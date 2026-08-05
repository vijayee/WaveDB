"""WaveDB Python bindings."""

from .config import VacuumConfig, VacuumMode, WaveDBConfig, WaveDBEncryption
from .database import WaveDB
from .exceptions import (
    EncryptionError,
    GraphQLLayerError,
    IOError_,
    InvalidPathError,
    NotFoundError,
    WaveDBError,
)
from .graph_layer import GraphLayer, GraphQuery, GraphResult
from .graphql_layer import GraphQLLayer, GraphQLError, GraphQLResult
from .log import LogLevel, set_log_callback, set_progress_handler, set_quiet
from .subtree import Subtree
from .vector_layer import (
    Distance,
    Format,
    IndexType,
    Runtime,
    VectorLayer,
    VectorResult,
)

try:
    from importlib.metadata import version as _version, PackageNotFoundError

    try:
        __version__ = _version("wavedb")
    except PackageNotFoundError:
        __version__ = "0.0.0+dev"
except ImportError:  # Python < 3.8 fallback (not supported, but be safe)
    __version__ = "0.0.0+dev"

__all__ = [
    "Distance",
    "EncryptionError",
    "Format",
    "GraphLayer",
    "GraphQuery",
    "GraphResult",
    "GraphQLError",
    "GraphQLLayer",
    "GraphQLLayerError",
    "GraphQLResult",
    "IOError_",
    "IndexType",
    "InvalidPathError",
    "LogLevel",
    "NotFoundError",
    "Runtime",
    "Subtree",
    "VacuumConfig",
    "VacuumMode",
    "VectorLayer",
    "VectorResult",
    "WaveDB",
    "WaveDBConfig",
    "WaveDBEncryption",
    "WaveDBError",
    "set_log_callback",
    "set_progress_handler",
    "set_quiet",
]
