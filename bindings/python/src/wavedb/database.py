"""WaveDB database class."""
from __future__ import annotations

from ._errors import map_error
from ._native import ffi, lib
from .config import WaveDBConfig, WaveDBEncryption
from .exceptions import InvalidPathError, WaveDBError


def _normalize_key(key, delimiter: str) -> bytes:
    if isinstance(key, list):
        if not key:
            raise InvalidPathError("key list must be non-empty")
        return delimiter.join(str(k) for k in key).encode("utf-8")
    if not isinstance(key, str):
        raise InvalidPathError(
            f"key must be str or list[str], got {type(key).__name__}"
        )
    if not key:
        raise InvalidPathError("key must be non-empty")
    return key.encode("utf-8")


def _encode_value(value) -> bytes:
    if isinstance(value, str):
        return value.encode("utf-8")
    if isinstance(value, (bytes, bytearray)):
        return bytes(value)
    raise TypeError(f"value must be str or bytes, got {type(value).__name__}")


_WAL_MODE_TO_U8 = {"debounced": 0, "immediate": 1, "none": 2}

# Return codes from the C sync raw API. The C layer uses:
#   0  -> success
#  -1  -> validation / argument error (NULL ptr, zero-length key, etc.)
#  -2  -> not found (database_get_sync returns -2 when no value is mapped)
_GET_NOT_FOUND_RC = -2


class WaveDB:
    """Synchronous WaveDB database handle."""

    def __init__(
        self,
        path: str,
        *,
        delimiter: str = "/",
        config: "WaveDBConfig | None" = None,
        encryption: "WaveDBEncryption | None" = None,
    ) -> None:
        if not path:
            raise InvalidPathError("path must be non-empty")
        if len(delimiter) != 1:
            raise InvalidPathError("delimiter must be a single character")

        self._delimiter = delimiter
        # cffi's ABI mode requires a bytes object of length 1 for the `char`
        # delimiter parameter — an int (from ord()) is rejected.
        self._delimiter_byte = delimiter.encode("utf-8")
        self._path = str(path)
        self._closed = False
        self._db = ffi.NULL

        err = ffi.new("int*")
        path_b = self._path.encode("utf-8")

        if encryption is not None:
            cfg = lib.encrypted_database_config_default()
            try:
                # NOTE: encrypted_database_config_set_type mapping is deferred
                # to Task 14 (encryption). We only wire up the key material
                # here; the default type from encrypted_database_config_default
                # is used until then.
                if encryption.symmetric_key is not None:
                    buf = ffi.from_buffer(encryption.symmetric_key)
                    lib.encrypted_database_config_set_symmetric_key(
                        cfg, buf, len(encryption.symmetric_key)
                    )
                if encryption.asymmetric_private_key is not None:
                    buf = ffi.from_buffer(encryption.asymmetric_private_key)
                    lib.encrypted_database_config_set_asymmetric_private_key(
                        cfg, buf, len(encryption.asymmetric_private_key)
                    )
                if encryption.asymmetric_public_key is not None:
                    buf = ffi.from_buffer(encryption.asymmetric_public_key)
                    lib.encrypted_database_config_set_asymmetric_public_key(
                        cfg, buf, len(encryption.asymmetric_public_key)
                    )
                self._db = lib.database_create_encrypted(path_b, cfg, err)
            finally:
                lib.encrypted_database_config_destroy(cfg)
        else:
            cfg = lib.database_config_default()
            try:
                # Only override C defaults when the caller passes an explicit
                # config. The WaveDBConfig dataclass defaults (worker_threads=0,
                # sync_only=False) are not by themselves sufficient to bring up
                # a concurrent database — database_create_with_config requires
                # either worker_threads > 0 or sync_only=True. The C
                # database_config_default() sets worker_threads=4 and
                # timer_resolution_ms=10, which is the right baseline when the
                # user does not customize anything.
                if config is not None:
                    c = config
                    lib.database_config_set_chunk_size(cfg, c.chunk_size)
                    lib.database_config_set_btree_node_size(
                        cfg, c.btree_node_size
                    )
                    lib.database_config_set_enable_persist(
                        cfg, 1 if c.enable_persist else 0
                    )
                    lib.database_config_set_lru_memory_mb(cfg, c.lru_memory_mb)
                    lib.database_config_set_lru_shards(cfg, c.lru_shards)
                    lib.database_config_set_wal_sync_mode(
                        cfg, _WAL_MODE_TO_U8[c.wal_sync_mode]
                    )
                    lib.database_config_set_wal_debounce_ms(
                        cfg, c.wal_debounce_ms
                    )
                    lib.database_config_set_worker_threads(cfg, c.worker_threads)
                    lib.database_config_set_sync_only(
                        cfg, 1 if c.sync_only else 0
                    )
                self._db = lib.database_create_with_config(path_b, cfg, err)
            finally:
                lib.database_config_destroy(cfg)

        if self._db == ffi.NULL:
            raise map_error(err[0], f"database_create failed for {self._path}")

    # ---- private helpers ----

    def _check_open(self) -> None:
        if self._closed or self._db == ffi.NULL:
            raise WaveDBError("DATABASE_CLOSED: database has been closed")

    def _raw_put(self, key_b: bytes, value_b: bytes) -> None:
        self._check_open()
        rc = lib.database_put_sync_raw(
            self._db,
            ffi.from_buffer(key_b), len(key_b), self._delimiter_byte,
            ffi.from_buffer(value_b), len(value_b),
        )
        if rc != 0:
            raise map_error(rc, "put_sync failed")

    def _raw_get(self, key_b: bytes):
        self._check_open()
        value_out = ffi.new("uint8_t**")
        len_out = ffi.new("size_t*")
        rc = lib.database_get_sync_raw(
            self._db,
            ffi.from_buffer(key_b), len(key_b), self._delimiter_byte,
            value_out, len_out,
        )
        if rc == 0:
            if value_out[0] == ffi.NULL:
                return None
            try:
                return ffi.buffer(value_out[0], len_out[0])[:]
            finally:
                lib.database_raw_value_free(value_out[0])
        if rc == _GET_NOT_FOUND_RC:
            return None
        raise map_error(rc, "get_sync failed")

    def _raw_del(self, key_b: bytes) -> None:
        self._check_open()
        rc = lib.database_delete_sync_raw(
            self._db,
            ffi.from_buffer(key_b), len(key_b), self._delimiter_byte,
        )
        if rc != 0:
            raise map_error(rc, "del_sync failed")

    # ---- public sync API ----

    def put_sync(self, key, value) -> None:
        """Store `value` at `key`. `key` may be a delimited str or list[str]."""
        self._raw_put(_normalize_key(key, self._delimiter), _encode_value(value))

    def get_sync(self, key):
        """Return the bytes stored at `key`, or None if not found."""
        return self._raw_get(_normalize_key(key, self._delimiter))

    def del_sync(self, key) -> None:
        """Delete the value at `key`. No-op if the key is absent."""
        self._raw_del(_normalize_key(key, self._delimiter))

    # ---- lifecycle ----

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        if self._db != ffi.NULL:
            lib.database_destroy(self._db)
            self._db = ffi.NULL

    def __enter__(self) -> "WaveDB":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass
