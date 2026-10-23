"""
deps.py - FastAPI auth dependencies.

require_device: CI-2, "the server shall authenticate each door device
before accepting any data from it. Data from an unrecognized device shall
be rejected." Every device-facing route in routers/device.py depends on
this - there is no route that accepts device data without it.

require_admin: SR-4, "the administration interface shall require
authentication." HTTP Basic over the admin API - simple and sufficient for
a single-site admin tool; swap for a real session/token scheme before
exposing this past a trusted network.
"""
import sqlite3
from datetime import datetime, timezone

from fastapi import Depends, Header, HTTPException, status
from fastapi.security import HTTPBasic, HTTPBasicCredentials

from . import security
from .db import get_db

_basic_auth = HTTPBasic()


def require_device(
    x_device_key: str | None = Header(None, description="Raw API key issued when the device was registered"),
    db: sqlite3.Connection = Depends(get_db),
) -> sqlite3.Row:
    if not x_device_key:
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "missing device key")  # CI-2: reject, same status as an unrecognized key
    devices = db.execute("SELECT * FROM devices").fetchall()
    for device in devices:
        if security.verify_secret(x_device_key, device["api_key_hash"]):
            if not device["enabled"]:
                # FR-38: "a disabled or removed node shall be refused by the server."
                raise HTTPException(status.HTTP_403_FORBIDDEN, "device disabled")
            db.execute(
                "UPDATE devices SET last_seen_at = ? WHERE id = ?",
                (datetime.now(timezone.utc).isoformat(), device["id"]),
            )
            return db.execute("SELECT * FROM devices WHERE id = ?", (device["id"],)).fetchone()
    # CI-2: unrecognized device -> reject, don't leak which part was wrong.
    raise HTTPException(status.HTTP_401_UNAUTHORIZED, "unrecognized device")


def require_admin(
    credentials: HTTPBasicCredentials = Depends(_basic_auth),
    db: sqlite3.Connection = Depends(get_db),
) -> sqlite3.Row:
    row = db.execute("SELECT * FROM admin_users WHERE username = ?", (credentials.username,)).fetchone()
    if row is None or not security.verify_secret(credentials.password, row["password_hash"]):
        raise HTTPException(
            status.HTTP_401_UNAUTHORIZED,
            "invalid admin credentials",
            headers={"WWW-Authenticate": "Basic"},
        )
    return row
