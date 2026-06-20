"""WaveDB Python bindings."""

from .config import WaveDBConfig, WaveDBEncryption
from .database import WaveDB
from .exceptions import (
    EncryptionError,
    IOError_,
    InvalidPathError,
    NotFoundError,
    WaveDBError,
)
from .subtree import Subtree

__version__ = "0.1.0"

__all__ = [
    "EncryptionError",
    "IOError_",
    "InvalidPathError",
    "NotFoundError",
    "Subtree",
    "WaveDB",
    "WaveDBError",
    "WaveDBConfig",
    "WaveDBEncryption",
]
