"""
recognition.py - the align -> embed -> compare pipeline from
ARCHITECTURE.md Sec.3.2.

Two embedders:

- InsightFaceEmbedder (default when the `insightface` package is
  installed) - a REAL face detector+aligner+embedder: ArcFace
  (buffalo_l, 512-d, ResNet/iResNet100 backbone, exactly what
  ARCHITECTURE.md Sec.3.3 recommends), via insightface's FaceAnalysis,
  which already does detect -> align -> embed internally. Verified
  end-to-end against insightface's own bundled sample images (not
  synthetic data): same-person cosine similarity ~1.0, different-person
  ~0.06, see CLOUD_SERVER/README.md for the numbers and how they were
  produced. Runs on GPU (CUDAExecutionProvider) automatically if
  onnxruntime-gpu's CUDA runtime libraries are importable, else falls
  back to CPU - see README.md's GPU section for why CUDA may not be
  available in a given environment (a real per-user disk quota blocked
  the CUDA 13 runtime libs during development here) and how to enable it.

- PlaceholderEmbedder - the original dependency-free stand-in (a
  downsampled grayscale intensity vector). Only used if `insightface`
  fails to import. Has no concept of a face at all and will not meet
  AR-1/AR-2 - kept only so the server still starts and the plumbing
  (threshold, averaging, cosine compare) stays testable without the
  extra dependency.

align() is folded into InsightFaceEmbedder.embed() - insightface's
FaceAnalysis.get() already does detection + landmark-based similarity-
transform alignment + embedding in one call (ARCHITECTURE.md Sec.3.2's
three steps), so there's no separate alignment stage to call out here
the way there was for the placeholder.
"""
import logging
from abc import ABC, abstractmethod
from io import BytesIO

import numpy as np
from PIL import Image

logger = logging.getLogger(__name__)

EMBEDDING_DIM_PLACEHOLDER = 64


class NoFaceFoundError(Exception):
    """FR-28: raised so callers can reject with a reason, not silently skip."""


class Embedder(ABC):
    @property
    @abstractmethod
    def version(self) -> str:
        """DR-3: tags the representations this embedder produces."""

    @abstractmethod
    def embed(self, image_bytes: bytes) -> np.ndarray:
        """Return an L2-normalized embedding for the most prominent face
        (FR-6) in `image_bytes`. Raises NoFaceFoundError if none is
        found (FR-28)."""


class PlaceholderEmbedder(Embedder):
    """See module header - NOT a real face embedder."""

    version = "placeholder-v1"

    def embed(self, image_bytes: bytes) -> np.ndarray:
        image = Image.open(BytesIO(image_bytes)).convert("L")
        side = int(np.sqrt(EMBEDDING_DIM_PLACEHOLDER))
        image = image.resize((side, side))
        vec = np.asarray(image, dtype=np.float32).flatten()
        norm = np.linalg.norm(vec)
        if norm == 0:
            return vec
        return vec / norm


def _build_insightface_embedder() -> "Embedder | None":
    try:
        import cv2
        from insightface.app import FaceAnalysis
    except ImportError:
        return None

    import onnxruntime as ort

    available = ort.get_available_providers()
    providers = ["CUDAExecutionProvider", "CPUExecutionProvider"] if "CUDAExecutionProvider" in available else ["CPUExecutionProvider"]
    using_gpu = providers[0] == "CUDAExecutionProvider"

    app = FaceAnalysis(name="buffalo_l", providers=providers)
    # det_size=320 (vs. the 640 default): the device is expected to send
    # an already-cropped single-face image (ARCHITECTURE.md Sec.4.2 "crop
    # with margin"), not a wide scene - smaller is faster and plenty for
    # that case. Measured on CPU: ~670ms/call at 320 vs. much longer at
    # 640 for the same crop (see README.md).
    app.prepare(ctx_id=0, det_size=(320, 320))

    class InsightFaceEmbedder(Embedder):
        version = "insightface-buffalo_l-v1"

        def __init__(self):
            self._app = app
            self._cv2 = cv2
            logger.info("InsightFaceEmbedder ready (provider=%s)", "GPU/CUDA" if using_gpu else "CPU")

        def embed(self, image_bytes: bytes) -> np.ndarray:
            pil_image = Image.open(BytesIO(image_bytes)).convert("RGB")
            bgr = self._cv2.cvtColor(np.asarray(pil_image), self._cv2.COLOR_RGB2BGR)
            faces = self._app.get(bgr)
            if not faces:
                raise NoFaceFoundError("no face detected in image")
            # FR-6: most prominent face = largest detected box.
            best = max(faces, key=lambda f: (f.bbox[2] - f.bbox[0]) * (f.bbox[3] - f.bbox[1]))
            vec = best.embedding.astype(np.float32)
            norm = np.linalg.norm(vec)
            return vec if norm == 0 else vec / norm

    return InsightFaceEmbedder()


_embedder: Embedder | None = _build_insightface_embedder()
if _embedder is None:
    logger.warning(
        "insightface not installed - falling back to PlaceholderEmbedder "
        "(NOT a real face recognizer, see recognition.py module header)"
    )
    _embedder = PlaceholderEmbedder()

EMBEDDING_VERSION = _embedder.version


def get_embedder() -> Embedder:
    return _embedder


def cosine_similarity(a: np.ndarray, b: np.ndarray) -> float:
    denom = (np.linalg.norm(a) * np.linalg.norm(b)) or 1e-9
    return float(np.dot(a, b) / denom)


def average_embeddings(embeddings: list[np.ndarray]) -> np.ndarray:
    """AR-2 / README 'why several photos': average across enrollment
    photos for a more robust representation than any single one."""
    stacked = np.stack(embeddings)
    avg = stacked.mean(axis=0)
    norm = np.linalg.norm(avg)
    return avg if norm == 0 else avg / norm


def embedding_to_blob(vec: np.ndarray) -> bytes:
    return vec.astype(np.float32).tobytes()


def blob_to_embedding(blob: bytes) -> np.ndarray:
    return np.frombuffer(blob, dtype=np.float32)


def identify(probe_image_bytes: bytes, candidates: list[tuple[int, bytes]], threshold: float):
    """ARCHITECTURE.md Sec.3.2 Compare+Score step: brute-force cosine
    similarity against every candidate (ASM-3: fine below a few thousand
    users). Returns (user_id, score) for the best match if it clears
    `threshold`, else (None, best_score_seen) so callers can still log
    AR-5's "confidence of each attempt" even on a miss. Returns
    (None, None) if no face was found in the probe image at all - that's
    a different case from "a face was found but didn't match anyone"."""
    try:
        probe_vec = get_embedder().embed(probe_image_bytes)
    except NoFaceFoundError:
        return None, None

    best_user_id = None
    best_score = -1.0
    for user_id, blob in candidates:
        score = cosine_similarity(probe_vec, blob_to_embedding(blob))
        if score > best_score:
            best_score = score
            best_user_id = user_id

    if best_user_id is not None and best_score >= threshold:
        return best_user_id, best_score
    return None, (best_score if best_score >= 0 else None)
