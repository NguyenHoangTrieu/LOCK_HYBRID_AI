"""
routers/device.py - the device-facing API (SRS Sec.5.1 interface table).
Every route here depends on require_device (CI-2) - no unauthenticated
device route exists.
"""
import sqlite3
from datetime import datetime, timezone

from fastapi import APIRouter, Depends, File, Form, HTTPException, UploadFile

from .. import attendance, cards, enrollment, inout, recognition
from ..db import get_db
from ..deps import require_device
from ..schemas import (
    BatchEventResult,
    BatchUploadRequest,
    BatchUploadResponse,
    CardAttendanceRequest,
    CardStoreEntry,
    CardStoreResponse,
    ConfigResponse,
    DecisionResponse,
    EnrollmentCardSubmitRequest,
    EnrollmentCardSubmitResponse,
    EnrollmentSessionOut,
    HeartbeatRequest,
    HeartbeatResponse,
)

router = APIRouter(prefix="/api/v1", tags=["device"])


def _card_decision_user(db: sqlite3.Connection, card_uid: str) -> sqlite3.Row | None:
    """FR-10a/FR-56/FR-58: the authoritative card decision - looked up
    through `cards` (not the users.card_uid mirror) so a lost or
    just-released card denies immediately even if the mirror hasn't
    propagated to a node's local store yet."""
    card = cards.get_card(db, card_uid)
    if card is None or card["state"] != "assigned" or card["user_id"] is None:
        return None
    return db.execute("SELECT * FROM users WHERE id = ?", (card["user_id"],)).fetchone()


def _config_response(db: sqlite3.Connection) -> ConfigResponse:
    return ConfigResponse(
        recognition_threshold=float(attendance.get_config(db, "recognition_threshold")),
        unlock_duration_sec=int(attendance.get_config(db, "unlock_duration_sec")),
        cooldown_sec=int(attendance.get_config(db, "cooldown_sec")),
        request_timeout_sec=int(attendance.get_config(db, "request_timeout_sec")),
        offline_card_access_enabled=attendance.get_config(db, "offline_card_access_enabled") == "true",
    )


@router.get("/config", response_model=ConfigResponse)
def get_config(device: sqlite3.Row = Depends(require_device), db: sqlite3.Connection = Depends(get_db)):
    """CI-4/MR-1: device fetches its operating parameters from the server."""
    return _config_response(db)


@router.post("/heartbeat", response_model=HeartbeatResponse)
def heartbeat(
    body: HeartbeatRequest | None = None,
    device: sqlite3.Row = Depends(require_device),
    db: sqlite3.Connection = Depends(get_db),
):
    """CI-3: require_device() already updated last_seen_at. CI-6/FR-39/
    FR-53: every heartbeat also doubles as the command channel - mode,
    any active enrollment session, and the current card_store_version
    all ride along on the reply the device was already going to poll
    for, rather than a separate push mechanism (there is no transport
    that supports the server initiating a message - see protocol.h)."""
    if body is not None:
        updates, params = [], []
        if body.fw_version is not None:
            updates.append("fw_version = ?")
            params.append(body.fw_version)
        if body.card_store_version is not None:
            updates.append("card_store_version = ?")
            params.append(body.card_store_version)
        if updates:
            params.append(device["id"])
            db.execute(f"UPDATE devices SET {', '.join(updates)} WHERE id = ?", params)

    session = enrollment.get_active_session(db, device) if device["mode"] == "enrollment" else None
    return HeartbeatResponse(
        ok=True,
        server_time=datetime.now(timezone.utc),
        mode=device["mode"],
        enrollment_session=EnrollmentSessionOut(**session) if session else None,
        card_store_version=cards.get_card_store_version(db),
    )


@router.get("/card_store", response_model=CardStoreResponse)
def get_card_store(device: sqlite3.Row = Depends(require_device), db: sqlite3.Connection = Depends(get_db)):
    """FR-51/FR-53: full snapshot of assigned-and-active cards. Always a
    full replace, never a delta - at ASM-3's scale (a few thousand cards)
    a full resync is simple and cheap enough that an incremental-update
    protocol would only add risk of the node's copy drifting silently."""
    return CardStoreResponse(
        version=cards.get_card_store_version(db),
        cards=[
            CardStoreEntry(uid=row["uid"], user_id=row["user_id"], name=row["user_name"])
            for row in cards.list_assigned_for_sync(db)
        ],
    )


@router.post("/enrollment/card", response_model=EnrollmentCardSubmitResponse)
def submit_enrollment_card(
    body: EnrollmentCardSubmitRequest,
    device: sqlite3.Row = Depends(require_device),
    db: sqlite3.Connection = Depends(get_db),
):
    """FR-48: card registration session. SR-10 is enforced by this route
    simply never touching the latch - only attendance_card()/
    attendance_face() ever lead to a grant."""
    if device["mode"] != "enrollment":
        raise HTTPException(400, "device is not in enrollment mode")
    session = enrollment.get_active_session(db, device)
    if session is None or session["type"] != "card":
        raise HTTPException(400, "no active card registration session")

    card = cards.get_card(db, body.card_uid)
    if card is not None and card["state"] == "assigned" and card["user_id"] == session["user_id"]:
        enrollment.cancel_session(db, device["id"])
        return EnrollmentCardSubmitResponse(result="already_registered")

    if card is not None and card["state"] == "assigned":
        holder = db.execute("SELECT name FROM users WHERE id = ?", (card["user_id"],)).fetchone()
        enrollment.cancel_session(db, device["id"])
        return EnrollmentCardSubmitResponse(result="needs_confirmation", holder_name=holder["name"] if holder else None)

    cards.assign_card(db, body.card_uid, session["user_id"], by=f"enrollment-session:{device['id']}")
    enrollment.cancel_session(db, device["id"])
    return EnrollmentCardSubmitResponse(result="assigned")


@router.post("/attendance/face", response_model=DecisionResponse)
def attendance_face(
    image: UploadFile = File(...),
    occurred_at: datetime = Form(...),
    device: sqlite3.Row = Depends(require_device),
    db: sqlite3.Connection = Depends(get_db),
):
    """FR-3..7: face crop in, decision out. FR-6 (most prominent face) is
    the device's job (it already only ever sends one crop, see
    ARCHITECTURE.md Sec.4.2) - this endpoint only ever evaluates the one
    image it's given."""
    image_bytes = image.file.read()
    threshold = float(attendance.get_config(db, "recognition_threshold"))

    candidates = db.execute(
        "SELECT id, embedding FROM users WHERE active = 1 AND embedding IS NOT NULL"
    ).fetchall()
    if not candidates:
        user_row = None
        confidence = None
    else:
        user_id, confidence = recognition.identify(
            image_bytes, [(row["id"], row["embedding"]) for row in candidates], threshold
        )
        user_row = (
            db.execute("SELECT * FROM users WHERE id = ?", (user_id,)).fetchone()
            if user_id is not None
            else None
        )

    result = attendance.decide_and_record(
        db,
        user_row=user_row,
        device_id=device["id"],
        method="face",
        confidence=confidence,
        occurred_at=occurred_at.isoformat(),
    )
    unlock_duration = int(attendance.get_config(db, "unlock_duration_sec")) if result["granted"] else 0
    # Note: Do NOT overwrite registered avatar with attendance camera capture (preserve enrolled photo)

    # UI-4/SR-8: clear in/out action message on success
    wire_name = result["name"]
    if result["granted"]:
        if result.get("direction") == "out" and wire_name:
            wire_name = f"OUT:{wire_name}"
            message = f"Goodbye, {result['name']}"
        else:
            message = f"Welcome, {result['name']}"
    else:
        message = "Not recognized"

    resp_dict = dict(result)
    resp_dict["name"] = wire_name
    return DecisionResponse(**resp_dict, unlock_duration_sec=unlock_duration, message=message)


@router.post("/attendance/card", response_model=DecisionResponse)
def attendance_card(
    body: CardAttendanceRequest,
    device: sqlite3.Row = Depends(require_device),
    db: sqlite3.Connection = Depends(get_db),
):
    """FR-9..11: card attendance evaluated centrally against cloud database."""
    user_row = _card_decision_user(db, body.card_uid)
    result = attendance.decide_and_record(
        db,
        user_row=user_row,
        device_id=device["id"],
        method="card",
        confidence=None,
        occurred_at=body.occurred_at.isoformat(),
    )
    unlock_duration = int(attendance.get_config(db, "unlock_duration_sec")) if result["granted"] else 0
    wire_name = result["name"]
    if result["granted"]:
        if result.get("direction") == "out" and wire_name:
            wire_name = f"OUT:{wire_name}"
            message = f"Goodbye, {result['name']}"
        else:
            message = f"Welcome, {result['name']}"
    else:
        message = "Access denied"

    resp_dict = dict(result)
    resp_dict["name"] = wire_name
    return DecisionResponse(**resp_dict, unlock_duration_sec=unlock_duration, message=message)


@router.post("/attendance/batch", response_model=BatchUploadResponse)
def attendance_batch(
    body: BatchUploadRequest,
    device: sqlite3.Row = Depends(require_device),
    db: sqlite3.Connection = Depends(get_db),
):
    """FR-20..22: offline-buffered events uploaded once connectivity
    returns. Only card events are meaningful here (FR-24: no offline face
    access) - a batched 'face' entry has no image attached by design
    (the device never buffers face crops offline) and is rejected rather
    than silently accepted. Each result is keyed by the device's own
    client_event_id so the device deletes only what the server actually
    confirmed (FR-22), never on a bare HTTP 200 for the whole batch."""
    results: list[BatchEventResult] = []
    for event in body.events:
        if event.method != "card" or not event.card_uid:
            results.append(BatchEventResult(client_event_id=event.client_event_id, accepted=False))
            continue

        user_row = _card_decision_user(db, event.card_uid)
        attendance.decide_and_record(
            db,
            user_row=user_row,
            device_id=device["id"],
            method="card",
            confidence=None,
            occurred_at=event.occurred_at.isoformat(),
        )
        results.append(BatchEventResult(client_event_id=event.client_event_id, accepted=True))

    return BatchUploadResponse(results=results)
