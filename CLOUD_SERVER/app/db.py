"""
db.py - SQLite schema and connection helper.

Plain sqlite3, no ORM - this is a single small-site deployment (ASM-3: a
few thousand users at most), not a scale target, so the extra dependency
and indirection of an ORM buys nothing here. One connection per request
(FastAPI dependency, see deps.py) rather than a pool - sqlite3 in WAL mode
handles that concurrency fine at this scale (PR-5: 10 devices).

Schema maps directly onto SRS Sec.3.3 (DR-1..DR-9):
  devices            - DR-5 (+ mode/enabled/fw_version/card_store_version,
                        FR-39..43, and an inline single-session enrollment
                        state, FR-45/46 - one row's worth of columns rather
                        than a separate table, since only one session is
                        ever active per device)
  users              - DR-1
  cards              - DR-8 (card_uid moved out of users; a card's
                        identity/state is independent of who currently
                        holds it, so reuse/release/lost (FR-55/58) have
                        somewhere to live that isn't "a column on a user")
  card_assignments   - FR-59 (assignment history)
  attendance_events  - DR-4
  admin_users        - SR-4 (admin auth, not in the SRS data model itself)
  config             - MR-1 (server-configurable thresholds/durations) +
                        the single global card_store_version counter
                        (FR-52)

users.card_uid is kept as a denormalized, read-only mirror of "the
currently-assigned card's UID, if any" (updated only by the card
helpers in attendance.py) purely so the existing card-lookup query
shape in routers/device.py keeps working without a rewrite; `cards` +
`card_assignments` are the actual source of truth for state/history.
"""
import os
import sqlite3
from contextlib import contextmanager
from datetime import datetime, timezone
from pathlib import Path

DB_PATH = Path(os.environ.get("LOCK_HYBRID_DB_PATH", Path(__file__).resolve().parent.parent / "data" / "lock_hybrid.db"))

SCHEMA = """
CREATE TABLE IF NOT EXISTS devices (
    id            TEXT PRIMARY KEY,
    name          TEXT NOT NULL,
    location      TEXT,
    api_key_hash  TEXT NOT NULL,
    last_seen_at  TEXT,
    created_at    TEXT NOT NULL,
    mode                 TEXT NOT NULL DEFAULT 'attendance' CHECK(mode IN ('attendance','enrollment')),
    enabled              INTEGER NOT NULL DEFAULT 1,
    fw_version           TEXT,
    card_store_version   INTEGER NOT NULL DEFAULT 0,
    session_active       INTEGER NOT NULL DEFAULT 0,
    session_type         TEXT CHECK(session_type IN ('card','face')),
    session_user_id      INTEGER REFERENCES users(id),
    session_started_at   TEXT,
    session_timeout_sec  INTEGER NOT NULL DEFAULT 60
);

CREATE TABLE IF NOT EXISTS users (
    id                 INTEGER PRIMARY KEY AUTOINCREMENT,
    name               TEXT NOT NULL,
    org_id             TEXT,
    active             INTEGER NOT NULL DEFAULT 1,
    card_uid           TEXT UNIQUE,
    embedding          BLOB,
    embedding_version  TEXT,
    created_at         TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS cards (
    uid         TEXT PRIMARY KEY,
    state       TEXT NOT NULL DEFAULT 'unassigned' CHECK(state IN ('unassigned','assigned','lost')),
    user_id     INTEGER REFERENCES users(id),
    created_at  TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS card_assignments (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    card_uid     TEXT NOT NULL REFERENCES cards(uid),
    user_id      INTEGER NOT NULL REFERENCES users(id),
    assigned_at  TEXT NOT NULL,
    released_at  TEXT,
    assigned_by  TEXT
);
CREATE INDEX IF NOT EXISTS idx_card_assignments_card ON card_assignments(card_uid);

CREATE TABLE IF NOT EXISTS attendance_events (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id      INTEGER REFERENCES users(id),
    device_id    TEXT NOT NULL REFERENCES devices(id),
    method       TEXT NOT NULL CHECK(method IN ('face','card')),
    outcome      TEXT NOT NULL CHECK(outcome IN ('granted','denied')),
    confidence   REAL,
    occurred_at  TEXT NOT NULL,
    received_at  TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_attendance_user_time ON attendance_events(user_id, occurred_at);
CREATE INDEX IF NOT EXISTS idx_attendance_device_time ON attendance_events(device_id, occurred_at);

CREATE TABLE IF NOT EXISTS admin_users (
    id             INTEGER PRIMARY KEY AUTOINCREMENT,
    username       TEXT UNIQUE NOT NULL,
    password_hash  TEXT NOT NULL,
    created_at     TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS config (
    key    TEXT PRIMARY KEY,
    value  TEXT NOT NULL
);
"""

# Columns added to `devices` after its original release - SQLite's
# CREATE TABLE IF NOT EXISTS above is a no-op against an already-existing
# table, so a DB created before this change needs these added explicitly
# (the systemd-deployed instance per WORKLOG.md is exactly that case).
_DEVICES_MIGRATION_COLUMNS = {
    "mode": "TEXT NOT NULL DEFAULT 'attendance'",
    "enabled": "INTEGER NOT NULL DEFAULT 1",
    "fw_version": "TEXT",
    "card_store_version": "INTEGER NOT NULL DEFAULT 0",
    "session_active": "INTEGER NOT NULL DEFAULT 0",
    "session_type": "TEXT",
    "session_user_id": "INTEGER REFERENCES users(id)",
    "session_started_at": "TEXT",
    "session_timeout_sec": "INTEGER NOT NULL DEFAULT 60",
}


def _migrate_devices_table(conn: sqlite3.Connection) -> None:
    existing = {row["name"] for row in conn.execute("PRAGMA table_info(devices)").fetchall()}
    for column, ddl in _DEVICES_MIGRATION_COLUMNS.items():
        if column not in existing:
            conn.execute(f"ALTER TABLE devices ADD COLUMN {column} {ddl}")


def _migrate_legacy_card_uids(conn: sqlite3.Connection) -> None:
    """One-time backfill: before this change, a card assignment was just
    `users.card_uid`. Populate `cards`/`card_assignments` from whatever is
    already there so DR-8/FR-59 have real history going forward, without
    losing pre-existing assignments. Idempotent (INSERT OR IGNORE on the
    card's primary key) - safe to run on every boot."""
    rows = conn.execute("SELECT id, card_uid FROM users WHERE card_uid IS NOT NULL").fetchall()
    now = datetime.now(timezone.utc).isoformat()
    for row in rows:
        conn.execute(
            "INSERT OR IGNORE INTO cards (uid, state, user_id, created_at) VALUES (?, 'assigned', ?, ?)",
            (row["card_uid"], row["id"], now),
        )
        existing = conn.execute(
            "SELECT 1 FROM card_assignments WHERE card_uid = ? AND user_id = ? AND released_at IS NULL",
            (row["card_uid"], row["id"]),
        ).fetchone()
        if existing is None:
            conn.execute(
                "INSERT INTO card_assignments (card_uid, user_id, assigned_at, assigned_by) VALUES (?, ?, ?, 'migration')",
                (row["card_uid"], row["id"], now),
            )

# MR-1: must be server-configurable without redeploying - these are the
# row-exists-or-not defaults seeded on first boot; admin.py's /config
# endpoint is what actually changes them afterwards.
DEFAULT_CONFIG = {
    "recognition_threshold": "0.6",   # AR-3: cosine-similarity cutoff, tune against real enrolled users
    "unlock_duration_sec": "4",
    "cooldown_sec": "300",
    "request_timeout_sec": "5",       # matches PR-1's 2s target plus margin, not identical to it
    "offline_card_access_enabled": "false",  # FR-23: disabled by default
    "card_store_version": "0",        # FR-52: bumped on every card-authorization change
}


def init_db() -> None:
    DB_PATH.parent.mkdir(parents=True, exist_ok=True)
    with get_connection() as conn:
        conn.executescript(SCHEMA)
        _migrate_devices_table(conn)
        for key, value in DEFAULT_CONFIG.items():
            conn.execute("INSERT OR IGNORE INTO config (key, value) VALUES (?, ?)", (key, value))
        _migrate_legacy_card_uids(conn)
        conn.commit()


@contextmanager
def get_connection():
    conn = sqlite3.connect(DB_PATH, timeout=10)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA foreign_keys = ON")
    conn.execute("PRAGMA journal_mode = WAL")
    try:
        yield conn
    finally:
        conn.close()


def get_db():
    """FastAPI dependency - one connection per request, committed/closed automatically."""
    with get_connection() as conn:
        yield conn
        conn.commit()
