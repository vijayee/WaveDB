"""Map C error codes and string markers to typed Python exceptions."""
from __future__ import annotations

from .exceptions import (
    EncryptionError,
    IOError_,
    InvalidPathError,
    NotFoundError,
    WaveDBError,
)


_DATABASE_ERR_ENCRYPTION_REQUIRED = -100
_DATABASE_ERR_ENCRYPTION_KEY_INVALID = -101
_DATABASE_ERR_ENCRYPTION_UNSUPPORTED = -102


def map_error(code: int, message: str) -> WaveDBError:
    msg = message or ""

    if code == _DATABASE_ERR_ENCRYPTION_REQUIRED:
        return EncryptionError(msg or "encryption required")
    if code == _DATABASE_ERR_ENCRYPTION_KEY_INVALID:
        return EncryptionError(msg or "invalid encryption key")
    if code == _DATABASE_ERR_ENCRYPTION_UNSUPPORTED:
        return EncryptionError(msg or "encryption unsupported")

    upper = msg.upper()
    if "NOT_FOUND" in upper or "KEY NOT FOUND" in upper:
        return NotFoundError(msg)
    if "INVALID_PATH" in upper or "INVALID PATH" in upper:
        return InvalidPathError(msg)
    if "IO_ERROR" in upper or "I/O ERROR" in upper or "DATABASE_CLOSED" in upper:
        return IOError_(msg)
    if "ENCRYPTION" in upper:
        return EncryptionError(msg)

    return WaveDBError(msg or f"WaveDB error (code {code})")


def raise_on_error(code: int, message_factory) -> None:
    """If code != 0, raise a mapped exception. message_factory is called only on error."""
    if code != 0:
        raise map_error(code, message_factory() if callable(message_factory) else str(message_factory))