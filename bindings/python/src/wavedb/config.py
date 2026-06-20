"""Configuration dataclasses for WaveDB."""
from __future__ import annotations

from dataclasses import dataclass


_VALID_WAL_MODES = {"debounced", "immediate", "none"}


@dataclass
class WaveDBConfig:
    chunk_size: int = 4
    btree_node_size: int = 4096
    enable_persist: bool = True
    lru_memory_mb: int = 50
    lru_shards: int = 0
    wal_sync_mode: str = "debounced"
    wal_debounce_ms: int = 250
    worker_threads: int = 0
    sync_only: bool = False

    def __post_init__(self) -> None:
        if self.wal_sync_mode not in _VALID_WAL_MODES:
            raise ValueError(f"wal_sync_mode must be one of {_VALID_WAL_MODES}")
        if self.chunk_size <= 0 or self.chunk_size > 255:
            raise ValueError("chunk_size must be in 1..255")


@dataclass
class WaveDBEncryption:
    type: str
    symmetric_key: bytes | None = None
    asymmetric_private_key: bytes | None = None
    asymmetric_public_key: bytes | None = None