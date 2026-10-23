"""
enrollment.py - Enrollment-mode sessions (FR-44..48). A session's state
lives directly on the owning device's row (session_active/type/user_id/
started_at/timeout_sec) rather than a separate table, since FR-46 caps
it at exactly one active session per device - there is never a
one-to-many relationship to model.

Only the card-registration session (FR-48, UI-20/21) is implemented
here. Face-enrollment sessions (FR-47, UI-18/19 - live camera preview
with pose guidance) are NOT implemented in this change; see WORKLOG.md.
"""
import sqlite3
from datetime import datetime, timedelta, timezone


class EnrollmentError(Exception):
    pass


def _is_expired(device: sqlite3.Row, now: datetime) -> bool:
    if not device["session_active"]:
        return False
    started = datetime.fromisoformat(device["session_started_at"])
    return (now - started).total_seconds() > device["session_timeout_sec"]


def get_active_session(db: sqlite3.Connection, device: sqlite3.Row) -> dict | None:
    """Lazily expires a session whose timeout has passed (FR-46) - no
    background job, just checked whenever the session is read."""
    if not device["session_active"]:
        return None
    now = datetime.now(timezone.utc)
    if _is_expired(device, now):
        cancel_session(db, device["id"])
        return None
    started = datetime.fromisoformat(device["session_started_at"])
    remaining = int(device["session_timeout_sec"] - (now - started).total_seconds())
    user = db.execute("SELECT name FROM users WHERE id = ?", (device["session_user_id"],)).fetchone()
    return {
        "type": device["session_type"],
        "user_id": device["session_user_id"],
        "user_name": user["name"] if user else None,
        "expires_in_sec": max(remaining, 0),
    }


def start_session(db: sqlite3.Connection, device: sqlite3.Row, *, user_id: int, session_type: str, timeout_sec: int) -> None:
    """FR-45: only an authenticated administrator starts a session, by
    picking a user and an Enrollment-mode node - never the node itself.
    FR-46: starting a new one implicitly ends whatever was active."""
    if device["mode"] != "enrollment":
        raise EnrollmentError("device is not in enrollment mode")
    user = db.execute("SELECT id FROM users WHERE id = ?", (user_id,)).fetchone()
    if user is None:
        raise EnrollmentError("user not found")
    if session_type != "card":
        raise EnrollmentError("only card registration sessions are implemented in this build")
    db.execute(
        """
        UPDATE devices SET session_active = 1, session_type = ?, session_user_id = ?,
               session_started_at = ?, session_timeout_sec = ? WHERE id = ?
        """,
        (session_type, user_id, datetime.now(timezone.utc).isoformat(), timeout_sec, device["id"]),
    )


def cancel_session(db: sqlite3.Connection, device_id: str) -> None:
    """FR-46: ends on completion, cancellation, or timeout - all three
    call this same cleanup."""
    db.execute(
        """
        UPDATE devices SET session_active = 0, session_type = NULL, session_user_id = NULL,
               session_started_at = NULL WHERE id = ?
        """,
        (device_id,),
    )
