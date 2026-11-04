"""Shared PWA password format; standard library only, no server dependencies."""
import hashlib
import secrets


def password_hash(password: str, salt: bytes) -> bytes:
    return hashlib.scrypt(password.encode("utf-8"), salt=salt, n=131072, r=8, p=1,
                          maxmem=256 * 1024 * 1024, dklen=32)


def password_record(password: str) -> dict:
    if not 16 <= len(password.encode("utf-8")) <= 256:
        raise ValueError("password must contain 16..256 UTF-8 bytes")
    salt = secrets.token_bytes(32)
    return {"v": 1, "algorithm": "scrypt-131072-8-1",
            "salt": salt.hex(), "hash": password_hash(password, salt).hex()}
