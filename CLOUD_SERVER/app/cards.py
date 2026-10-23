"""
cards.py - card authorization data: assign/release/mark-lost (FR-55,
FR-58), assignment history (FR-59, DR-8), and the single global
card_store_version counter (FR-52) that tells nodes their local card
store (FR-51) is stale.

`users.card_uid` is kept as a denormalized mirror of "this card's
current holder" purely for the existing card-lookup query shape in
routers/device.py - every function here is the only thing allowed to
write it, so it never drifts from `cards`/`card_assignments`.
"""
import sqlite3
from datetime import datetime, timezone


class CardError(Exception):
    """Raised for a card operation that is a client mistake (bad state
    transition), not a server fault - callers turn this into a 409/404."""


def get_card_store_version(db: sqlite3.Connection) -> int:
    row = db.execute("SELECT value FROM config WHERE key = 'card_store_version'").fetchone()
    return int(row["value"]) if row is not None else 0


def _bump_card_store_version(db: sqlite3.Connection) -> int:
    """FR-52: incremented on every change (assign/release/reassign/lost/
    user activation/deactivation/rename/delete). Callers of this module
    cover the card-side changes; user activation/rename/delete call this
    directly from admin.py (see there)."""
    new_version = get_card_store_version(db) + 1
    db.execute("UPDATE config SET value = ? WHERE key = 'card_store_version'", (str(new_version),))
    return new_version


def bump_card_store_version(db: sqlite3.Connection) -> int:
    """Public entry point for callers outside this module (admin.py's
    user activate/deactivate/rename/delete - FR-52 lists those as
    version-bumping events too, even though they don't touch `cards`)."""
    return _bump_card_store_version(db)


def get_card(db: sqlite3.Connection, uid: str) -> sqlite3.Row | None:
    return db.execute("SELECT * FROM cards WHERE uid = ?", (uid,)).fetchone()


def list_cards(db: sqlite3.Connection) -> list[sqlite3.Row]:
    return db.execute(
        """
        SELECT c.uid, c.state, c.user_id, u.name AS user_name, c.created_at
        FROM cards c LEFT JOIN users u ON u.id = c.user_id
        ORDER BY c.created_at DESC
        """
    ).fetchall()


def list_assigned_for_sync(db: sqlite3.Connection) -> list[sqlite3.Row]:
    """FR-51: what an Attendance-mode node's local card store should
    contain - only assigned cards whose holder is active (a deactivated
    user's card must not grant access, FR-30/FR-56, and the simplest way
    to guarantee that from a node that only ever does a local lookup is
    to never hand it a card that shouldn't work)."""
    return db.execute(
        """
        SELECT c.uid, c.user_id, u.name AS user_name
        FROM cards c JOIN users u ON u.id = c.user_id
        WHERE c.state = 'assigned' AND u.active = 1
        ORDER BY c.uid
        """
    ).fetchall()


def _set_user_card_uid_mirror(db: sqlite3.Connection, user_id: int, card_uid: str | None) -> None:
    db.execute("UPDATE users SET card_uid = ? WHERE id = ?", (card_uid, user_id))


def release_card(db: sqlite3.Connection, uid: str, *, by: str) -> None:
    """FR-55 release: back to the unassigned pool. FR-57: past attendance
    records are untouched (they reference user_id directly, not the
    card)."""
    card = get_card(db, uid)
    if card is None or card["state"] != "assigned":
        raise CardError("card is not currently assigned")
    now = datetime.now(timezone.utc).isoformat()
    db.execute(
        "UPDATE card_assignments SET released_at = ? WHERE card_uid = ? AND released_at IS NULL",
        (now, uid),
    )
    db.execute("UPDATE cards SET state = 'unassigned', user_id = NULL WHERE uid = ?", (uid,))
    _set_user_card_uid_mirror(db, card["user_id"], None)
    _bump_card_store_version(db)


def assign_card(db: sqlite3.Connection, uid: str, user_id: int, *, by: str, confirm_reassign: bool = False) -> sqlite3.Row:
    """FR-48/FR-55 assign or reassign. A card already held by someone
    else requires `confirm_reassign=True` (UI-25's confirmation) - without
    it this raises CardError so the caller can show who holds it first."""
    user = db.execute("SELECT * FROM users WHERE id = ?", (user_id,)).fetchone()
    if user is None:
        raise CardError("user not found")

    card = get_card(db, uid)
    now = datetime.now(timezone.utc).isoformat()

    if card is None:
        db.execute("INSERT INTO cards (uid, state, user_id, created_at) VALUES (?, 'unassigned', NULL, ?)", (uid, now))
        card = get_card(db, uid)

    if card["state"] == "lost":
        raise CardError("card is marked lost - clear that first")

    if card["state"] == "assigned":
        if card["user_id"] == user_id:
            return card  # FR-48: "already registered to this user" - no-op
        if not confirm_reassign:
            raise CardError("card is assigned to another user - confirmation required to reassign")
        db.execute(
            "UPDATE card_assignments SET released_at = ? WHERE card_uid = ? AND released_at IS NULL",
            (now, uid),
        )

    db.execute("UPDATE cards SET state = 'assigned', user_id = ? WHERE uid = ?", (user_id, uid))
    db.execute(
        "INSERT INTO card_assignments (card_uid, user_id, assigned_at, assigned_by) VALUES (?, ?, ?, ?)",
        (uid, user_id, now, by),
    )
    _set_user_card_uid_mirror(db, user_id, uid)
    _bump_card_store_version(db)
    return get_card(db, uid)


def mark_lost(db: sqlite3.Connection, uid: str, *, by: str) -> None:
    """FR-58: denied everywhere immediately, not assignable until
    unmarked. Unlike release(), the card keeps pointing at its last
    holder (so "unmark" can restore it without re-asking who it was)."""
    card = get_card(db, uid)
    if card is None:
        raise CardError("card not found")
    if card["state"] == "assigned":
        db.execute(
            "UPDATE card_assignments SET released_at = ? WHERE card_uid = ? AND released_at IS NULL",
            (datetime.now(timezone.utc).isoformat(), uid),
        )
        _set_user_card_uid_mirror(db, card["user_id"], None)
    db.execute("UPDATE cards SET state = 'lost' WHERE uid = ?", (uid,))
    _bump_card_store_version(db)


def unmark_lost(db: sqlite3.Connection, uid: str) -> None:
    """FR-58: "until the mark is removed" - returns the card to the
    unassigned pool; the holder it last had must be re-assigned
    explicitly (the assignment was already closed out by mark_lost())."""
    card = get_card(db, uid)
    if card is None or card["state"] != "lost":
        raise CardError("card is not marked lost")
    db.execute("UPDATE cards SET state = 'unassigned', user_id = NULL WHERE uid = ?", (uid,))
    _bump_card_store_version(db)


def release_card_for_user(db: sqlite3.Connection, user_id: int, *, by: str) -> None:
    """FR-31: deleting a user releases whatever card they held."""
    card = db.execute("SELECT uid FROM cards WHERE user_id = ? AND state = 'assigned'", (user_id,)).fetchone()
    if card is not None:
        release_card(db, card["uid"], by=by)
