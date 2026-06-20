"""Configuration dataclasses for WaveDB."""
from __future__ import annotations

from dataclasses import dataclass


_VALID_WAL_MODES = frozenset({"debounced", "immediate", "none"})


@dataclass
class WaveDBConfig:
    chunk_size: int = 4
    btree_node_size: int = 4096
    enable_persist: bool = True
    lru_memory_mb: int = 50
    lru_shards: int = 0
    wal_sync_mode: str = "debounced"
    wal_debounce_ms: int = 250
    worker_threads: int = 4  # C default; 0 requires sync_only=True
    sync_only: bool = False

    def __post_init__(self) -> None:
        if self.wal_sync_mode not in _VALID_WAL_MODES:
            raise ValueError(f"wal_sync_mode must be one of {_VALID_WAL_MODES}")
        if self.chunk_size <= 0 or self.chunk_size > 255:
            raise ValueError("chunk_size must be in 1..255")
        if self.btree_node_size <= 0:
            raise ValueError("btree_node_size must be positive")
        if self.lru_memory_mb < 0:
            raise ValueError("lru_memory_mb must be non-negative")
        if self.lru_shards < 0:
            raise ValueError("lru_shards must be non-negative")
        if self.wal_debounce_ms < 0:
            raise ValueError("wal_debounce_ms must be non-negative")
        if self.worker_threads < 0 or self.worker_threads > 255:
            raise ValueError("worker_threads must be in 0..255")


@dataclass
class WaveDBEncryption:
    type: str
    symmetric_key: bytes | None = None
    asymmetric_private_key: bytes | None = None
    asymmetric_public_key: bytes | None = None

    def __post_init__(self) -> None:
        if not isinstance(self.type, str) or not self.type:
            raise ValueError("WaveDBEncryption.type must be a non-empty string")
