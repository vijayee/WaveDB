"""WaveDB Python bindings."""

from .exceptions import (
    EncryptionError,
    IOError_,
    InvalidPathError,
    NotFoundError,
    WaveDBError,
)

__version__ = "0.1.0"

__all__ = [
    "EncryptionError",
    "IOError_",
    "InvalidPathError",
    "NotFoundError",
    "WaveDBError",
]