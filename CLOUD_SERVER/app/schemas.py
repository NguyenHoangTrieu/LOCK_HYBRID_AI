"""schemas.py - request/response models for the device and admin APIs."""
from datetime import datetime

from pydantic import BaseModel, Field


class DecisionResponse(BaseModel):
    granted: bool
    name: str | None = None
    unlock_duration_sec: int = 0
    message: str


class CardAttendanceRequest(BaseModel):
    card_uid: str
    occurred_at: datetime


class BatchEvent(BaseModel):
    method: str = Field(pattern="^(face|card)$")
    card_uid: str | None = None
    occurred_at: datetime
    client_event_id: str  # device-generated id, echoed back so it can delete only confirmed events (FR-22)


class BatchEventResult(BaseModel):
    client_event_id: str
    accepted: bool


class BatchUploadRequest(BaseModel):
    events: list[BatchEvent]


class BatchUploadResponse(BaseModel):
    results: list[BatchEventResult]


class ConfigResponse(BaseModel):
    recognition_threshold: float
    unlock_duration_sec: int
    cooldown_sec: int
    request_timeout_sec: int
    offline_card_access_enabled: bool


class EnrollmentSessionOut(BaseModel):
    type: str
    user_id: int
    user_name: str | None
    expires_in_sec: int


class HeartbeatRequest(BaseModel):
    """FR-41/FR-54a: the device reports what it currently has so the
    admin UI (FR-43) can show which nodes are out of date - optional so
    a bare `POST /heartbeat` (no body) still works for older firmware."""
    fw_version: str | None = None
    card_store_version: int | None = None


class HeartbeatResponse(BaseModel):
    ok: bool
    server_time: datetime
    mode: str  # CI-4/FR-39: device learns its current mode from every heartbeat
    enrollment_session: EnrollmentSessionOut | None = None  # FR-45, when mode == "enrollment"
    card_store_version: int  # FR-52/CI-6: the server's current target version, so the device knows to sync


class DeviceCreateRequest(BaseModel):
    name: str
    location: str | None = None


class DeviceCreateResponse(BaseModel):
    id: str
    api_key: str  # shown exactly once


class DeviceOut(BaseModel):
    id: str
    name: str
    location: str | None
    last_seen_at: str | None
    mode: str
    enabled: bool
    fw_version: str | None
    card_store_version: int
    enrollment_session: EnrollmentSessionOut | None = None


class DeviceUpdateRequest(BaseModel):
    mode: str | None = Field(default=None, pattern="^(attendance|enrollment)$")
    enabled: bool | None = None


class CardOut(BaseModel):
    uid: str
    state: str
    user_id: int | None
    user_name: str | None
    created_at: str


class CardAssignRequest(BaseModel):
    user_id: int
    confirm_reassign: bool = False


class CardStoreEntry(BaseModel):
    uid: str
    user_id: int
    name: str


class CardStoreResponse(BaseModel):
    version: int
    cards: list[CardStoreEntry]


class EnrollmentStartRequest(BaseModel):
    user_id: int
    type: str = Field(pattern="^card$")  # face sessions not implemented in this build
    timeout_sec: int = 60


class EnrollmentCardSubmitRequest(BaseModel):
    card_uid: str


class EnrollmentCardSubmitResponse(BaseModel):
    result: str  # "assigned" | "already_registered" | "needs_confirmation"
    holder_name: str | None = None


class UserCreateResponse(BaseModel):
    id: int
    name: str


class UserOut(BaseModel):
    id: int
    name: str
    org_id: str | None
    active: bool
    card_uid: str | None
    has_face_enrollment: bool


class AttendanceOut(BaseModel):
    id: int
    user_id: int | None
    user_name: str | None
    device_id: str
    method: str
    outcome: str
    confidence: float | None
    occurred_at: str
    received_at: str


class ConfigUpdateRequest(BaseModel):
    recognition_threshold: float | None = None
    unlock_duration_sec: int | None = None
    cooldown_sec: int | None = None
    request_timeout_sec: int | None = None
    offline_card_access_enabled: bool | None = None
