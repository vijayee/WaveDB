"""Configuration dataclasses for WaveDB."""
from __future__ import annotations

import enum
from dataclasses import dataclass


_VALID_WAL_MODES = frozenset({"debounced", "immediate", "none"})


class VacuumMode(enum.IntEnum):
    """Vacuum scheduling modes (mirrors C `vacuum_mode_t`).

    `manual_only` (0): vacuum only runs when the caller invokes `WaveDB.vacuum()`.
    `strict` (1): auto-vacuum triggers as soon as the stale-byte ratio exceeds
                 `stale_threshold` (default mode).
    `adaptive` (2): like `strict` but defers vacuum while writers are active,
                    bounded by `adaptive_busy_threshold`.
    """

    manual_only = 0
    strict = 1
    adaptive = 2


@dataclass
class VacuumConfig:
    """Vacuum configuration (mirrors C `vacuum_config_t`).

    All times are in milliseconds. Defaults match the C defaults in
    `database_config_default()`.
    """

    mode: VacuumMode = VacuumMode.strict
    stale_threshold: float = 0.30
    min_file_size_bytes: int = 64 * 1024 * 1024
    min_stale_bytes: int = 16 * 1024 * 1024
    background_interval_ms: int = 60000
    drain_timeout_ms: int = 5000
    cursor_close_wait_ms: int = 60000
    max_runtime_ms: int = 30000
    writer_block_timeout_ms: int = 0
    adaptive_busy_threshold: int = 32

    def __post_init__(self) -> None:
        # Accept raw ints so callers can pass `VacuumMode.manual_only` or `0`.
        self.mode = VacuumMode(int(self.mode))
        if not 0.0 <= self.stale_threshold <= 1.0:
            raise ValueError("stale_threshold must be in [0.0, 1.0]")
        if self.min_file_size_bytes < 0:
            raise ValueError("min_file_size_bytes must be non-negative")
        if self.min_stale_bytes < 0:
            raise ValueError("min_stale_bytes must be non-negative")
        if self.background_interval_ms < 0:
            raise ValueError("background_interval_ms must be non-negative")
        if self.drain_timeout_ms < 0:
            raise ValueError("drain_timeout_ms must be non-negative")
        if self.cursor_close_wait_ms < 0:
            raise ValueError("cursor_close_wait_ms must be non-negative")
        if self.max_runtime_ms < 0:
            raise ValueError("max_runtime_ms must be non-negative")
        if self.writer_block_timeout_ms < 0:
            raise ValueError("writer_block_timeout_ms must be non-negative")
        if self.adaptive_busy_threshold < 0:
            raise ValueError("adaptive_busy_threshold must be non-negative")

    def apply_to(self, lib, c_config) -> None:
        """Push fields into a cffi `database_config_t*` via the C setters.

        cffi declares `database_config_t` as an opaque struct in this binding,
        so we cannot touch `c_config.vacuum_config.*` directly — we must go
        through the `database_config_set_vacuum_*` functions added in Task 18.
        """
        lib.database_config_set_vacuum_mode(c_config, int(self.mode))
        lib.database_config_set_vacuum_stale_threshold(c_config, float(self.stale_threshold))
        lib.database_config_set_vacuum_min_file_size_bytes(c_config, int(self.min_file_size_bytes))
        lib.database_config_set_vacuum_min_stale_bytes(c_config, int(self.min_stale_bytes))
        lib.database_config_set_vacuum_background_interval_ms(c_config, int(self.background_interval_ms))
        lib.database_config_set_vacuum_drain_timeout_ms(c_config, int(self.drain_timeout_ms))
        lib.database_config_set_vacuum_cursor_close_wait_ms(c_config, int(self.cursor_close_wait_ms))
        lib.database_config_set_vacuum_max_runtime_ms(c_config, int(self.max_runtime_ms))
        lib.database_config_set_vacuum_writer_block_timeout_ms(c_config, int(self.writer_block_timeout_ms))
        lib.database_config_set_vacuum_adaptive_busy_threshold(c_config, int(self.adaptive_busy_threshold))


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
    in_memory: bool = False  # True = pass location=NULL to C (no WAL, no page file, truly ephemeral)

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
