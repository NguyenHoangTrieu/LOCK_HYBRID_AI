"""
attendance.py - FR-12..15: record every attempt, apply the cooldown
without ever using it as a reason to deny access.
"""
import sqlite3
from datetime import datetime, timezone


def get_config(db: sqlite3.Connection, key: str) -> str:
    row = db.execute("SELECT value FROM config WHERE key = ?", (key,)).fetchone()
    if row is None:
        raise KeyError(key)
    return row["value"]


def within_cooldown(db: sqlite3.Connection, user_id: int, device_id: str, cooldown_sec: int) -> bool:
    row = db.execute(
        """
        SELECT occurred_at FROM attendance_events
        WHERE user_id = ? AND outcome = 'granted'
        ORDER BY occurred_at DESC LIMIT 1
        """,
        (user_id,),
    ).fetchone()
    if row is None:
        return False
    try:
        last = datetime.fromisoformat(row["occurred_at"])
    except Exception:
        return False
    now = datetime.now(timezone.utc)
    if last.tzinfo is None:
        last = last.replace(tzinfo=timezone.utc)
    return abs((now - last).total_seconds()) < cooldown_sec


def determine_direction(db: sqlite3.Connection, user_id: int, occurred_at: str) -> str:
    """First scan of the day is 'in' (check-in).
    Subsequent scan alternates: in -> out -> in -> out."""
    try:
        dt = datetime.fromisoformat(occurred_at)
        if dt.tzinfo is None:
            dt = dt.replace(tzinfo=timezone.utc)
        dt_local = dt.astimezone()
        start_utc = dt_local.replace(hour=0, minute=0, second=0, microsecond=0).astimezone(timezone.utc).isoformat()
        end_utc = dt_local.replace(hour=23, minute=59, second=59, microsecond=999999).astimezone(timezone.utc).isoformat()
    except Exception:
        start_utc = occurred_at[:10] + "T00:00:00"
        end_utc = occurred_at[:10] + "T23:59:59"

    last_event = db.execute(
        """
        SELECT direction FROM attendance_events
        WHERE user_id = ? AND outcome = 'granted' AND occurred_at >= ? AND occurred_at <= ?
        ORDER BY occurred_at DESC, id DESC LIMIT 1
        """,
        (user_id, start_utc, end_utc),
    ).fetchone()

    if last_event is None or last_event["direction"] is None or last_event["direction"] == "out":
        return "in"
    return "out"


def record_event(
    db: sqlite3.Connection,
    *,
    user_id: int | None,
    device_id: str,
    method: str,
    outcome: str,
    confidence: float | None,
    occurred_at: str,
    direction: str | None = None,
) -> int:
    """FR-15: `occurred_at` is caller-supplied (the device's own clock,
    CI-5), not derived from receipt time - `received_at` records that
    separately so neither is lost (DR-4)."""
    cur = db.execute(
        """
        INSERT INTO attendance_events (user_id, device_id, method, outcome, confidence, occurred_at, received_at, direction)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        """,
        (user_id, device_id, method, outcome, confidence, occurred_at, datetime.now(timezone.utc).isoformat(), direction),
    )
    return cur.lastrowid


def decide_and_record(
    db: sqlite3.Connection,
    *,
    user_row: sqlite3.Row | None,
    device_id: str,
    method: str,
    confidence: float | None,
    occurred_at: str,
) -> dict:
    """Shared FR-11/FR-13/FR-14 decision+bookkeeping for both face and card
    attempts: not found/inactive -> denied+logged; found+active -> granted,
    with a new record only if outside cooldown (a repeat within cooldown
    still grants, it just doesn't duplicate the record)."""
    if user_row is None or not user_row["active"]:
        record_event(
            db,
            user_id=(user_row["id"] if user_row is not None else None),
            device_id=device_id,
            method=method,
            outcome="denied",
            confidence=confidence,
            occurred_at=occurred_at,
            direction=None,
        )
        return {"granted": False, "name": None, "direction": None}

    cooldown_sec = int(get_config(db, "cooldown_sec"))
    # Card tapping needs only a short 3-second debounce; face can use full cooldown_sec
    cooldown = min(cooldown_sec, 3) if method == "card" else cooldown_sec

    if within_cooldown(db, user_row["id"], device_id, cooldown):
        # Repeat attempt within cooldown: do not log duplicate, and do not alternate direction!
        # Return the currently active direction for this user.
        last_event = db.execute(
            """
            SELECT direction FROM attendance_events
            WHERE user_id = ? AND outcome = 'granted'
            ORDER BY occurred_at DESC, id DESC LIMIT 1
            """,
            (user_row["id"],),
        ).fetchone()
        current_dir = last_event["direction"] if (last_event and last_event["direction"]) else "in"
        return {"granted": True, "name": user_row["name"], "direction": current_dir}

    direction = determine_direction(db, user_row["id"], occurred_at)
    record_event(
        db,
        user_id=user_row["id"],
        device_id=device_id,
        method=method,
        outcome="granted",
        confidence=confidence,
        occurred_at=occurred_at,
        direction=direction,
    )

    return {"granted": True, "name": user_row["name"], "direction": direction}
