"""
main.py - FastAPI app entrypoint.

Run with: uvicorn app.main:app --reload (see ../README.md).
"""
import os
import secrets
from contextlib import asynccontextmanager
from datetime import datetime, timezone
from pathlib import Path

from fastapi import FastAPI
from fastapi.staticfiles import StaticFiles

from . import security
from .db import get_connection, init_db
from .routers import admin, device

STATIC_DIR = Path(__file__).resolve().parent / "static"


def _bootstrap_admin() -> None:
    """First-boot convenience: if there is no admin account yet, create
    one so the admin web app (SR-4) is reachable at all. Prints the
    generated password to the server's own console/log exactly once -
    never stored in plaintext, never sent anywhere."""
    with get_connection() as conn:
        existing = conn.execute("SELECT COUNT(*) AS n FROM admin_users").fetchone()
        if existing["n"] > 0:
            return
        username = os.environ.get("LOCK_HYBRID_ADMIN_USER", "admin")
        password = os.environ.get("LOCK_HYBRID_ADMIN_PASSWORD") or secrets.token_urlsafe(12)
        conn.execute(
            "INSERT INTO admin_users (username, password_hash, created_at) VALUES (?, ?, ?)",
            (username, security.hash_secret(password), datetime.now(timezone.utc).isoformat()),
        )
        conn.commit()
        print("=" * 72)
        print(f"Bootstrap admin account created: username={username!r}")
        if "LOCK_HYBRID_ADMIN_PASSWORD" not in os.environ:
            print(f"Generated password: {password}")
            print("(set LOCK_HYBRID_ADMIN_PASSWORD to choose your own instead)")
        print("=" * 72)


@asynccontextmanager
async def lifespan(app: FastAPI):
    init_db()
    _bootstrap_admin()
    yield


app = FastAPI(
    title="LOCK_HYBRID_AI Cloud Backend",
    description="Recognition/attendance API for the Edge AI Node + admin web app.",
    lifespan=lifespan,
)

app.include_router(device.router)
app.include_router(admin.router)

if STATIC_DIR.exists():
    app.mount("/admin", StaticFiles(directory=STATIC_DIR, html=True), name="admin-ui")


@app.get("/healthz")
def healthz():
    return {"ok": True}
