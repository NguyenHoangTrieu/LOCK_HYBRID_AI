"""
routers/admin.py - the admin web app's API (SRS Sec.3.2.6/3.2.7, SR-4/SR-5).
Every route depends on require_admin.
"""
import csv
import io
import sqlite3
from datetime import datetime, timezone

from fastapi import APIRouter, Depends, File, HTTPException, Response, UploadFile
from fastapi.responses import FileResponse, StreamingResponse

from .. import cards as cards_mod
from .. import enrollment, inout, recognition, security
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

public_router = APIRouter(prefix="/admin/api", tags=["admin-public"])
router = APIRouter(prefix="/admin/api", tags=["admin"], dependencies=[Depends(require_admin)])


@public_router.get("/users/{user_id}/avatar")
def get_user_avatar(user_id: int, db: sqlite3.Connection = Depends(get_db)):
    """Public endpoint to serve user avatar image or dynamic SVG fallback for HTML <img> tags."""
    path = inout.get_avatar_file(user_id)
    if path is not None:
        return FileResponse(path, media_type="image/jpeg")

    row = db.execute("SELECT name FROM users WHERE id = ?", (user_id,)).fetchone()
    name = row["name"] if row else ""
    svg_bytes = inout.generate_default_avatar_svg(name, user_id)
    return Response(content=svg_bytes, media_type="image/svg+xml")


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
def create_user(
    name: str,
    org_id: str | None = None,
    card_uid: str | None = None,
    db: sqlite3.Connection = Depends(get_db),
    admin: sqlite3.Row = Depends(require_admin),
):
    """FR-26: register a user with name, optional org_id, and optional card_uid."""
    name_clean = name.strip()
    if not name_clean:
        raise HTTPException(400, "name cannot be empty")
    cur = db.execute(
        "INSERT INTO users (name, org_id, active, created_at) VALUES (?, ?, 1, ?)",
        (name_clean, org_id.strip() if org_id else None, datetime.now(timezone.utc).isoformat()),
    )
    user_id = cur.lastrowid
    if card_uid and card_uid.strip():
        try:
            cards_mod.assign_card(db, card_uid.strip(), user_id, by=admin["username"], confirm_reassign=True)
        except Exception:
            pass
    return UserCreateResponse(id=user_id, name=name_clean)


@router.post("/users/{user_id}/photos")
def enroll_photos(user_id: int, photos: list[UploadFile], db: sqlite3.Connection = Depends(get_db)):
    """FR-27/FR-28/DR-2: accepts several photos, averages their
    embeddings, stores only the averaged embedding, and saves the first photo as registered avatar."""
    user = db.execute("SELECT * FROM users WHERE id = ?", (user_id,)).fetchone()
    if user is None:
        raise HTTPException(404, "user not found")
    if not photos:
        raise HTTPException(400, "at least one photo is required")

    embeddings = []
    saved_avatar = False
    for photo in photos:
        data = photo.file.read()
        try:
            vec = recognition.get_embedder().embed(data)
        except recognition.NoFaceFoundError:
            raise HTTPException(400, f"no face found in '{photo.filename}'")
        embeddings.append(vec)
        if not saved_avatar:
            inout.save_avatar_image(user_id, data)
            saved_avatar = True

    averaged = recognition.average_embeddings(embeddings)
    db.execute(
        "UPDATE users SET embedding = ?, embedding_version = ? WHERE id = ?",
        (recognition.embedding_to_blob(averaged), recognition.EMBEDDING_VERSION, user_id),
    )
    return {"ok": True, "photos_used": len(embeddings)}


@router.post("/users/{user_id}/avatar")
def upload_user_avatar(user_id: int, avatar: UploadFile = File(...), db: sqlite3.Connection = Depends(get_db)):
    """Upload or update profile picture avatar for a user and sync face embedding if face detected."""
    user = db.execute("SELECT * FROM users WHERE id = ?", (user_id,)).fetchone()
    if user is None:
        raise HTTPException(404, "user not found")
    data = avatar.file.read()
    if not inout.save_avatar_image(user_id, data):
        raise HTTPException(400, "invalid or unreadable image file")

    face_enrolled = False
    try:
        vec = recognition.get_embedder().embed(data)
        db.execute(
            "UPDATE users SET embedding = ?, embedding_version = ? WHERE id = ?",
            (recognition.embedding_to_blob(vec), recognition.EMBEDDING_VERSION, user_id),
        )
        face_enrolled = True
    except Exception:
        pass

    return {"ok": True, "face_enrolled": face_enrolled}


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
    name: str | None = None,
    org_id: str | None = None,
    active: bool | None = None,
    card_uid: str | None = None,
    admin: sqlite3.Row = Depends(require_admin),
    db: sqlite3.Connection = Depends(get_db),
):
    """Update user information: name, org_id, active status, or assigned card_uid."""
    user = db.execute("SELECT * FROM users WHERE id = ?", (user_id,)).fetchone()
    if user is None:
        raise HTTPException(404, "user not found")

    if name is not None:
        name_clean = name.strip()
        if not name_clean:
            raise HTTPException(400, "name cannot be empty")
        db.execute("UPDATE users SET name = ? WHERE id = ?", (name_clean, user_id))

    if org_id is not None:
        db.execute("UPDATE users SET org_id = ? WHERE id = ?", (org_id.strip() or None, user_id))

    if active is not None:
        db.execute("UPDATE users SET active = ? WHERE id = ?", (int(active), user_id))
        cards_mod.bump_card_store_version(db)

    if card_uid is not None:
        card_clean = card_uid.strip()
        current_uid = user["card_uid"] or ""
        if card_clean != current_uid:
            try:
                if card_clean == "":
                    cards_mod.release_card_for_user(db, user_id, by=admin["username"])
                else:
                    cards_mod.assign_card(db, card_clean, user_id, by=admin["username"], confirm_reassign=True)
            except cards_mod.CardError as exc:
                raise HTTPException(409, str(exc))
    return {"ok": True}


@router.delete("/users/{user_id}")
def delete_user(user_id: int, admin: sqlite3.Row = Depends(require_admin), db: sqlite3.Connection = Depends(get_db)):
    """FR-31/SR-5: permanent delete of the user and their data."""
    cards_mod.release_card_for_user(db, user_id, by=admin["username"])
    inout.delete_avatar_file(user_id)
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


@router.delete("/devices/{device_id}")
def delete_device(
    device_id: str,
    admin: sqlite3.Row = Depends(require_admin),
    db: sqlite3.Connection = Depends(get_db),
):
    """Deletes a device and its attendance records and active enrollment sessions."""
    device = db.execute("SELECT * FROM devices WHERE id = ?", (device_id,)).fetchone()
    if device is None:
        raise HTTPException(404, "device not found")
    enrollment.cancel_session(db, device_id)
    db.execute("DELETE FROM attendance_events WHERE device_id = ?", (device_id,))
    db.execute("DELETE FROM devices WHERE id = ?", (device_id,))
    return {"ok": True}


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
            direction=r["direction"],
        )
        for r in rows
    ]


@router.get("/inout")
def get_inout_status(date: str | None = None, db: sqlite3.Connection = Depends(get_db)):
    """Daily Check-In / Check-Out monitor: personnel, photo, status (IN/OUT), first in, last out, duration."""
    target_date = date or datetime.now().strftime("%Y-%m-%d")
    return inout.get_inout_status_for_date(db, target_date)


@router.get("/inout/export.xlsx")
def export_inout_excel(
    from_date: str | None = None,
    to_date: str | None = None,
    db: sqlite3.Connection = Depends(get_db),
):
    """Multi-day attendance report export as Excel (.xlsx)."""
    today_str = datetime.now().strftime("%Y-%m-%d")
    f_date = from_date or today_str
    t_date = to_date or today_str
    content = inout.generate_inout_excel(db, f_date, t_date)
    filename = f"inout_report_{f_date}_to_{t_date}.xlsx"
    return Response(
        content=content,
        media_type="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
        headers={"Content-Disposition": f"attachment; filename={filename}"},
    )


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
