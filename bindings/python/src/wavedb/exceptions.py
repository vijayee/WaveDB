"""WaveDB exception hierarchy."""


class WaveDBError(Exception):
    """Base class for all WaveDB errors."""


class NotFoundError(WaveDBError):
    """Raised when a key is not found (only when caller opts in)."""


class InvalidPathError(WaveDBError):
    """Raised when a path is empty or malformed."""


class IOError_(WaveDBError):
    """Raised on I/O failures and closed-database accesses."""


class EncryptionError(WaveDBError):
    """Raised when encryption is required, unsupported, or the key is invalid."""


class GraphQLLayerError(WaveDBError):
    """Raised when GraphQL schema parsing or query execution fails."""
