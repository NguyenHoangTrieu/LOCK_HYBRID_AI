# Cloud Backend

FastAPI + SQLite implementation of the server half of `ARCHITECTURE.md`
Sec.3: device authentication, the face/card attendance API, the
recognition pipeline, attendance records with cooldown, and an admin web
app (enrollment, cards, reports, config). Verified end-to-end against a
running instance (device registration, card + face attendance, cooldown,
deactivation, delete, offline batch upload, CSV export, config update,
and every auth-rejection path) - not just written against the spec.

## Status / what to know before relying on this

**Recognition now uses a real model: ArcFace (insightface's `buffalo_l`,
512-d, exactly what `ARCHITECTURE.md` Sec.3.3 recommends), not the old
placeholder.** `app/recognition.py`'s `InsightFaceEmbedder` does
detect+align+embed in one call via `insightface`'s `FaceAnalysis`.
Verified against insightface's own bundled sample photos (not synthetic
data, not a cherry-picked claim):

| Test | Cosine similarity |
|---|---|
| Same person (two crops of the same face) | 0.9999 |
| Two different people | 0.065 |

...and through the actual HTTP API end to end: enrolled one face crop,
then confirmed `/api/v1/attendance/face` grants for that same person,
denies for a different person, and denies gracefully (no crash) for an
image with no face at all - plus enrollment correctly rejects a no-face
photo with a 400 (FR-28). The default `recognition_threshold` (0.6)
sits comfortably between those two numbers, but **has not been tuned
against a real population** - AR-4 still requires testing under the real
deployment's lighting/camera before trusting any FAR/FRR number.

If `insightface` isn't installed, `recognition.py` logs a warning and
falls back to the old `PlaceholderEmbedder` (grayscale pixel vector, no
concept of a face) so the server still starts - don't rely on recognition
quality in that mode.

**GPU**: `InsightFaceEmbedder` auto-selects `CUDAExecutionProvider` when
available, else CPU. Measured on this machine: **~670ms per call on CPU**
(16 cores, single pre-cropped face, `det_size=320`) - workable but above
PR-4's 500ms target under load. GPU wasn't reachable here because
`onnxruntime-gpu` 1.31's CUDA 13 runtime libraries (~1.2GB of
`nvidia-cublas`/`nvidia-cudnn`/`nvidia-curand`) hit a **per-user disk
quota** on this machine mid-download, not a code or driver problem (the
RTX 3050 and its driver are fine - `nvidia-smi` reports CUDA 13.2). Free
up space and `pip install nvidia-cublas nvidia-cuda-runtime nvidia-curand
nvidia-cudnn --extra-index-url https://pypi.nvidia.com` to finish enabling
it - no code change needed, the provider selection is already automatic.

Everything else - auth, DB schema, attendance/cooldown logic, offline
batch handling, admin CRUD, config - is real, not a stub.

## Running it

```bash
cd CLOUD_SERVER
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
uvicorn app.main:app --reload
```

First start creates an admin account and prints its password once to the
console (or set `LOCK_HYBRID_ADMIN_PASSWORD` yourself beforehand). Admin
web app: `http://localhost:8000/admin/`. API docs (FastAPI's own):
`http://localhost:8000/docs`.

Data lives in `data/lock_hybrid.db` (gitignored) - delete it to reset.
Override the path with `LOCK_HYBRID_DB_PATH`.

## Running it persistently on this machine (systemd --user)

`deploy/lock-hybrid-backend.service` is a user-level (no root) systemd
unit - runs the exact venv/uvicorn command above, restarts on crash,
starts automatically once enabled. Nothing here is applied
automatically - review the file, then:

```bash
mkdir -p ~/.config/systemd/user
cp CLOUD_SERVER/deploy/lock-hybrid-backend.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now lock-hybrid-backend
# so it also starts on boot without you being logged in yet:
loginctl enable-linger "$USER"
```

`journalctl --user -u lock-hybrid-backend -f` to follow logs (including
the one-time bootstrap admin password on first start, if you didn't set
`LOCK_HYBRID_ADMIN_PASSWORD`). `systemctl --user stop/disable
lock-hybrid-backend` to undo.

## Layout

```
app/
  main.py          - app wiring, bootstrap admin account
  db.py             - SQLite schema (DR-1..DR-7) + connection helper
  security.py        - PBKDF2 password/API-key hashing (stdlib only)
  deps.py              - require_device (CI-2), require_admin (SR-4)
  recognition.py        - align/embed/compare pipeline (see Status above)
  attendance.py           - cooldown (FR-13/14) + record writing (FR-12/15)
  schemas.py                - request/response models
  routers/device.py          - /api/v1/* - the Edge Node's own interface
  routers/admin.py            - /admin/api/* - enrollment, cards, reports, config
  static/index.html            - the admin web app (vanilla JS, no build step)
```

## Device API (`/api/v1/*`, requires `X-Device-Key`)

| Route | Maps to |
|---|---|
| `GET /config` | CI-4/MR-1 - threshold, durations, offline-card toggle |
| `POST /heartbeat` | CI-3 |
| `POST /attendance/face` | FR-3..7 |
| `POST /attendance/card` | FR-9..11 |
| `POST /attendance/batch` | FR-20..22 (offline-buffered card events only, FR-24) |

## Admin API (`/admin/api/*`, HTTP Basic)

Users (FR-26..31), devices (DR-5, issues the `X-Device-Key` once),
attendance listing + CSV export (FR-32/33/35), config (MR-1).

## Not implemented here

- GPU acceleration isn't active yet on this machine (disk quota - see
  Status above); the code path is ready, just unexercised with CUDA.
- TLS termination (CI-1) - this serves plain HTTP; put a reverse proxy
  (nginx/caddy) with a real certificate in front of it for any real
  deployment, same as any FastAPI app.
- Multi-admin roles/permissions - one flat admin_users table, no RBAC.
- Face alignment is only as good as insightface's own landmark-based
  similarity transform (built into `FaceAnalysis.get()`) - no custom
  alignment tuning has been done.
