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
    last = datetime.fromisoformat(row["occurred_at"])
    now = datetime.now(timezone.utc)
    return (now - last).total_seconds() < cooldown_sec


def record_event(
    db: sqlite3.Connection,
    *,
    user_id: int | None,
    device_id: str,
    method: str,
    outcome: str,
    confidence: float | None,
    occurred_at: str,
) -> int:
    """FR-15: `occurred_at` is caller-supplied (the device's own clock,
    CI-5), not derived from receipt time - `received_at` records that
    separately so neither is lost (DR-4)."""
    cur = db.execute(
        """
        INSERT INTO attendance_events (user_id, device_id, method, outcome, confidence, occurred_at, received_at)
        VALUES (?, ?, ?, ?, ?, ?, ?)
        """,
        (user_id, device_id, method, outcome, confidence, occurred_at, datetime.now(timezone.utc).isoformat()),
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
        )
        return {"granted": False, "name": None}

    cooldown_sec = int(get_config(db, "cooldown_sec"))
    if not within_cooldown(db, user_row["id"], device_id, cooldown_sec):
        record_event(
            db,
            user_id=user_row["id"],
            device_id=device_id,
            method=method,
            outcome="granted",
            confidence=confidence,
            occurred_at=occurred_at,
        )

    return {"granted": True, "name": user_row["name"]}
