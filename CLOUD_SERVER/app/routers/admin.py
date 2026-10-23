"""
routers/admin.py - the admin web app's API (SRS Sec.3.2.6/3.2.7, SR-4/SR-5).
Every route depends on require_admin.
"""
import csv
import io
import sqlite3
from datetime import datetime, timezone

from fastapi import APIRouter, Depends, HTTPException, UploadFile
from fastapi.responses import StreamingResponse

from .. import cards as cards_mod
from .. import enrollment, recognition, security
from ..db import get_db
from ..deps import require_admin
from ..schemas import (
    AttendanceOut,
    CardAssignRequest,
    CardOut,
    ConfigResponse,
    ConfigUpdateRequest,
    DeviceCreateRequest,
    DeviceCreateResponse,
    DeviceOut,
    DeviceUpdateRequest,
    EnrollmentSessionOut,
    EnrollmentStartRequest,
    UserCreateResponse,
    UserOut,
)

router = APIRouter(prefix="/admin/api", tags=["admin"], dependencies=[Depends(require_admin)])


def _device_out(db: sqlite3.Connection, row: sqlite3.Row) -> DeviceOut:
    session = enrollment.get_active_session(db, row) if row["mode"] == "enrollment" else None
    return DeviceOut(
        id=row["id"],
        name=row["name"],
        location=row["location"],
        last_seen_at=row["last_seen_at"],
        mode=row["mode"],
        enabled=bool(row["enabled"]),
        fw_version=row["fw_version"],
        card_store_version=row["card_store_version"],
        enrollment_session=EnrollmentSessionOut(**session) if session else None,
    )


# ---------------------------------------------------------------------- #
# Users / enrollment (FR-26..31, DR-2)
# ---------------------------------------------------------------------- #


@router.post("/users", response_model=UserCreateResponse)
def create_user(name: str, org_id: str | None, db: sqlite3.Connection = Depends(get_db)):
    """FR-26: register a user without face photos yet - UR-3's 3-minute
    enroll budget is spent in the UI flow (name+org, then photos via
    POST /users/{id}/photos), not necessarily one HTTP call."""
    cur = db.execute(
        "INSERT INTO users (name, org_id, active, created_at) VALUES (?, ?, 1, ?)",
        (name, org_id, datetime.now(timezone.utc).isoformat()),
    )
    return UserCreateResponse(id=cur.lastrowid, name=name)


@router.post("/users/{user_id}/photos")
def enroll_photos(user_id: int, photos: list[UploadFile], db: sqlite3.Connection = Depends(get_db)):
    """FR-27/FR-28/DR-2: accepts several photos, averages their
    embeddings, stores only the averaged embedding, discards the photo
    bytes once this function returns (they are never written anywhere -
    DR-2's 'original enrollment images shall not be retained')."""
    user = db.execute("SELECT * FROM users WHERE id = ?", (user_id,)).fetchone()
    if user is None:
        raise HTTPException(404, "user not found")
    if not photos:
        raise HTTPException(400, "at least one photo is required")

    embeddings = []
    for photo in photos:
        data = photo.file.read()
        try:
            vec = recognition.get_embedder().embed(data)
        except recognition.NoFaceFoundError:
            # FR-28: reject with a reason, don't silently skip a bad photo.
            raise HTTPException(400, f"no face found in '{photo.filename}'")
        embeddings.append(vec)

    averaged = recognition.average_embeddings(embeddings)
    db.execute(
        "UPDATE users SET embedding = ?, embedding_version = ? WHERE id = ?",
        (recognition.embedding_to_blob(averaged), recognition.EMBEDDING_VERSION, user_id),
    )
    return {"ok": True, "photos_used": len(embeddings)}


@router.get("/users", response_model=list[UserOut])
def list_users(db: sqlite3.Connection = Depends(get_db)):
    rows = db.execute("SELECT * FROM users ORDER BY name").fetchall()
    return [
        UserOut(
            id=r["id"],
            name=r["name"],
            org_id=r["org_id"],
            active=bool(r["active"]),
            card_uid=r["card_uid"],
            has_face_enrollment=r["embedding"] is not None,
        )
        for r in rows
    ]


@router.patch("/users/{user_id}")
def update_user(
    user_id: int,
    active: bool | None = None,
    card_uid: str | None = None,
    admin: sqlite3.Row = Depends(require_admin),
    db: sqlite3.Connection = Depends(get_db),
):
    """FR-29 (assign/revoke a card through the `cards` helpers, so
    DR-8/FR-59 history stays accurate even via this quick-assign route -
    pass card_uid="" to release) and FR-30 (deactivate: active=False
    denies both methods immediately; also bumps card_store_version
    (FR-52) since a deactivated user's card must stop working on nodes
    within PR-9, same as an explicit release)."""
    user = db.execute("SELECT * FROM users WHERE id = ?", (user_id,)).fetchone()
    if user is None:
        raise HTTPException(404, "user not found")

    if active is not None:
        db.execute("UPDATE users SET active = ? WHERE id = ?", (int(active), user_id))
        cards_mod.bump_card_store_version(db)
    if card_uid is not None:
        try:
            if card_uid == "":
                cards_mod.release_card_for_user(db, user_id, by=admin["username"])
            else:
                cards_mod.assign_card(db, card_uid, user_id, by=admin["username"])
        except cards_mod.CardError as exc:
            raise HTTPException(409, str(exc))  # DR-6 / UI-25 confirmation cases
    return {"ok": True}


@router.delete("/users/{user_id}")
def delete_user(user_id: int, admin: sqlite3.Row = Depends(require_admin), db: sqlite3.Connection = Depends(get_db)):
    """FR-31/SR-5: permanent delete of the user and their data. Attendance
    records keep the row but it now points at a vanished user_id (DR-4
    still wants 'which person where identified' as a historical fact);
    SQLite's default FK behavior leaves user_id as-is rather than cascading,
    which is what we want here - the attendance history isn't the personal
    data SR-5 is about, the user row (name/org/embedding/card) is.
    FR-31: any card they held is released (FR-56)."""
    cards_mod.release_card_for_user(db, user_id, by=admin["username"])
    db.execute("UPDATE attendance_events SET user_id = NULL WHERE user_id = ?", (user_id,))
    db.execute("DELETE FROM users WHERE id = ?", (user_id,))
    return {"ok": True}


# ---------------------------------------------------------------------- #
# Devices (DR-5, FR-34)
# ---------------------------------------------------------------------- #


@router.post("/devices", response_model=DeviceCreateResponse)
def create_device(body: DeviceCreateRequest, db: sqlite3.Connection = Depends(get_db)):
    device_id = security.generate_device_id()
    api_key = security.generate_api_key()
    db.execute(
        "INSERT INTO devices (id, name, location, api_key_hash, created_at) VALUES (?, ?, ?, ?, ?)",
        (device_id, body.name, body.location, security.hash_secret(api_key), datetime.now(timezone.utc).isoformat()),
    )
    return DeviceCreateResponse(id=device_id, api_key=api_key)  # shown once - not recoverable afterwards


@router.get("/devices", response_model=list[DeviceOut])
def list_devices(db: sqlite3.Connection = Depends(get_db)):
    rows = db.execute("SELECT * FROM devices ORDER BY name").fetchall()
    return [_device_out(db, r) for r in rows]


@router.patch("/devices/{device_id}", response_model=DeviceOut)
def update_device(device_id: str, body: DeviceUpdateRequest, db: sqlite3.Connection = Depends(get_db)):
    """FR-38/FR-39: mode and enabled are the only two settings an
    administrator changes here. FR-42 (a mode change doesn't interrupt
    an in-progress identification/enrollment) is the node's own
    responsibility - it applies the new mode next time it returns to
    idle (CI-9 keeps the wire format transport-independent either way)."""
    device = db.execute("SELECT * FROM devices WHERE id = ?", (device_id,)).fetchone()
    if device is None:
        raise HTTPException(404, "device not found")
    if body.mode is not None:
        db.execute("UPDATE devices SET mode = ? WHERE id = ?", (body.mode, device_id))
        if body.mode == "attendance":
            enrollment.cancel_session(db, device_id)  # leaving enrollment mode ends any session (FR-44)
    if body.enabled is not None:
        db.execute("UPDATE devices SET enabled = ? WHERE id = ?", (int(body.enabled), device_id))
    return _device_out(db, db.execute("SELECT * FROM devices WHERE id = ?", (device_id,)).fetchone())


# ---------------------------------------------------------------------- #
# Cards (FR-55..59, DR-8)
# ---------------------------------------------------------------------- #


@router.get("/cards", response_model=list[CardOut])
def list_cards(db: sqlite3.Connection = Depends(get_db)):
    return [
        CardOut(uid=r["uid"], state=r["state"], user_id=r["user_id"], user_name=r["user_name"], created_at=r["created_at"])
        for r in cards_mod.list_cards(db)
    ]


@router.post("/cards/{uid}/assign", response_model=CardOut)
def assign_card(uid: str, body: CardAssignRequest, admin: sqlite3.Row = Depends(require_admin), db: sqlite3.Connection = Depends(get_db)):
    try:
        card = cards_mod.assign_card(db, uid, body.user_id, by=admin["username"], confirm_reassign=body.confirm_reassign)
    except cards_mod.CardError as exc:
        raise HTTPException(409, str(exc))  # UI-25: caller shows this and asks the admin to confirm
    row = db.execute(
        "SELECT c.*, u.name AS user_name FROM cards c LEFT JOIN users u ON u.id = c.user_id WHERE c.uid = ?", (uid,)
    ).fetchone()
    return CardOut(uid=row["uid"], state=row["state"], user_id=row["user_id"], user_name=row["user_name"], created_at=row["created_at"])


@router.post("/cards/{uid}/release")
def release_card(uid: str, admin: sqlite3.Row = Depends(require_admin), db: sqlite3.Connection = Depends(get_db)):
    try:
        cards_mod.release_card(db, uid, by=admin["username"])
    except cards_mod.CardError as exc:
        raise HTTPException(409, str(exc))
    return {"ok": True}


@router.post("/cards/{uid}/lost")
def mark_card_lost(uid: str, admin: sqlite3.Row = Depends(require_admin), db: sqlite3.Connection = Depends(get_db)):
    try:
        cards_mod.mark_lost(db, uid, by=admin["username"])
    except cards_mod.CardError as exc:
        raise HTTPException(409, str(exc))
    return {"ok": True}


@router.post("/cards/{uid}/unlost")
def unmark_card_lost(uid: str, db: sqlite3.Connection = Depends(get_db)):
    try:
        cards_mod.unmark_lost(db, uid)
    except cards_mod.CardError as exc:
        raise HTTPException(409, str(exc))
    return {"ok": True}


# ---------------------------------------------------------------------- #
# Enrollment sessions (FR-45/46, UI-17/20/21)
# ---------------------------------------------------------------------- #


@router.post("/devices/{device_id}/enrollment/start", response_model=EnrollmentSessionOut)
def start_enrollment(device_id: str, body: EnrollmentStartRequest, db: sqlite3.Connection = Depends(get_db)):
    device = db.execute("SELECT * FROM devices WHERE id = ?", (device_id,)).fetchone()
    if device is None:
        raise HTTPException(404, "device not found")
    try:
        enrollment.start_session(db, device, user_id=body.user_id, session_type=body.type, timeout_sec=body.timeout_sec)
    except enrollment.EnrollmentError as exc:
        raise HTTPException(400, str(exc))
    device = db.execute("SELECT * FROM devices WHERE id = ?", (device_id,)).fetchone()
    return EnrollmentSessionOut(**enrollment.get_active_session(db, device))


@router.post("/devices/{device_id}/enrollment/cancel")
def cancel_enrollment(device_id: str, db: sqlite3.Connection = Depends(get_db)):
    enrollment.cancel_session(db, device_id)
    return {"ok": True}


# ---------------------------------------------------------------------- #
# Attendance / reporting (FR-32..35)
# ---------------------------------------------------------------------- #


@router.get("/attendance", response_model=list[AttendanceOut])
def list_attendance(
    user_id: int | None = None,
    start: datetime | None = None,
    end: datetime | None = None,
    outcome: str | None = None,
    db: sqlite3.Connection = Depends(get_db),
):
    """FR-32 (filter by user/date range) and FR-35 (denied attempts
    visible too - outcome='denied' filters to just those)."""
    query = """
        SELECT a.*, u.name AS user_name FROM attendance_events a
        LEFT JOIN users u ON u.id = a.user_id
        WHERE 1=1
    """
    params: list = []
    if user_id is not None:
        query += " AND a.user_id = ?"
        params.append(user_id)
    if start is not None:
        query += " AND a.occurred_at >= ?"
        params.append(start.isoformat())
    if end is not None:
        query += " AND a.occurred_at <= ?"
        params.append(end.isoformat())
    if outcome is not None:
        query += " AND a.outcome = ?"
        params.append(outcome)
    query += " ORDER BY a.occurred_at DESC LIMIT 1000"

    rows = db.execute(query, params).fetchall()
    return [
        AttendanceOut(
            id=r["id"],
            user_id=r["user_id"],
            user_name=r["user_name"],
            device_id=r["device_id"],
            method=r["method"],
            outcome=r["outcome"],
            confidence=r["confidence"],
            occurred_at=r["occurred_at"],
            received_at=r["received_at"],
        )
        for r in rows
    ]


@router.get("/attendance/export.csv")
def export_attendance_csv(db: sqlite3.Connection = Depends(get_db)):
    """FR-33: export in a spreadsheet-friendly format."""
    rows = db.execute(
        """
        SELECT a.*, u.name AS user_name FROM attendance_events a
        LEFT JOIN users u ON u.id = a.user_id
        ORDER BY a.occurred_at DESC
        """
    ).fetchall()

    buffer = io.StringIO()
    writer = csv.writer(buffer)
    writer.writerow(["id", "user_name", "device_id", "method", "outcome", "confidence", "occurred_at", "received_at"])
    for r in rows:
        writer.writerow(
            [r["id"], r["user_name"], r["device_id"], r["method"], r["outcome"], r["confidence"], r["occurred_at"], r["received_at"]]
        )
    buffer.seek(0)
    return StreamingResponse(
        buffer,
        media_type="text/csv",
        headers={"Content-Disposition": "attachment; filename=attendance_export.csv"},
    )


# ---------------------------------------------------------------------- #
# Config (MR-1)
# ---------------------------------------------------------------------- #


@router.get("/config", response_model=ConfigResponse)
def get_config(db: sqlite3.Connection = Depends(get_db)):
    from .. import attendance as attendance_mod

    return ConfigResponse(
        recognition_threshold=float(attendance_mod.get_config(db, "recognition_threshold")),
        unlock_duration_sec=int(attendance_mod.get_config(db, "unlock_duration_sec")),
        cooldown_sec=int(attendance_mod.get_config(db, "cooldown_sec")),
        request_timeout_sec=int(attendance_mod.get_config(db, "request_timeout_sec")),
        offline_card_access_enabled=attendance_mod.get_config(db, "offline_card_access_enabled") == "true",
    )


@router.put("/config", response_model=ConfigResponse)
def update_config(body: ConfigUpdateRequest, db: sqlite3.Connection = Depends(get_db)):
    """MR-1: recognition threshold, unlock duration, cooldown, timeout,
    and offline-card-access all change here - no device reprogram/redeploy."""
    updates = body.model_dump(exclude_none=True)
    for key, value in updates.items():
        stored = "true" if value is True else "false" if value is False else str(value)
        db.execute("UPDATE config SET value = ? WHERE key = ?", (stored, key))
    return get_config(db)
