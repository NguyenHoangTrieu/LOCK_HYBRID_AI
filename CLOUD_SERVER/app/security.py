"""
security.py - password/API-key hashing. stdlib only (hashlib.pbkdf2_hmac) -
no passlib/bcrypt dependency for a project this size.

Device API keys (CI-2) and admin passwords (SR-4) use the same scheme:
random secret issued/chosen once, only a salted PBKDF2 hash is ever stored,
the raw value is shown to the caller exactly once (at creation) and never
again.
"""
import hashlib
import hmac
import os
import secrets

_PBKDF2_ITERATIONS = 200_000


def _hash(secret: str, salt: bytes) -> bytes:
    return hashlib.pbkdf2_hmac("sha256", secret.encode("utf-8"), salt, _PBKDF2_ITERATIONS)


def hash_secret(secret: str) -> str:
    salt = os.urandom(16)
    digest = _hash(secret, salt)
    return f"{salt.hex()}${digest.hex()}"


def verify_secret(secret: str, stored: str) -> bool:
    try:
        salt_hex, digest_hex = stored.split("$", 1)
    except ValueError:
        return False
    salt = bytes.fromhex(salt_hex)
    expected = bytes.fromhex(digest_hex)
    actual = _hash(secret, salt)
    return hmac.compare_digest(actual, expected)


def generate_api_key() -> str:
    return secrets.token_urlsafe(32)


def generate_device_id(prefix: str = "dev") -> str:
    return f"{prefix}_{secrets.token_hex(4)}"
