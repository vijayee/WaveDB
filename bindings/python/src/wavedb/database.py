"""WaveDB database class."""
from __future__ import annotations

from ._async import AsyncBridge
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
        delimiter_bytes = delimiter.encode("utf-8")
        if len(delimiter_bytes) != 1:
            raise InvalidPathError("delimiter must encode to a single byte")
        self._delimiter = delimiter
        # cffi's ABI mode requires a bytes object of length 1 for the `char`
        # delimiter parameter — an int (from ord()) is rejected.
        self._delimiter_byte = delimiter_bytes
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
                c = config or WaveDBConfig()
                lib.database_config_set_chunk_size(cfg, c.chunk_size)
                lib.database_config_set_btree_node_size(cfg, c.btree_node_size)
                lib.database_config_set_enable_persist(cfg, 1 if c.enable_persist else 0)
                lib.database_config_set_lru_memory_mb(cfg, c.lru_memory_mb)
                lib.database_config_set_lru_shards(cfg, c.lru_shards)
                lib.database_config_set_wal_sync_mode(cfg, _WAL_MODE_TO_U8[c.wal_sync_mode])
                lib.database_config_set_wal_debounce_ms(cfg, c.wal_debounce_ms)
                lib.database_config_set_worker_threads(cfg, c.worker_threads)
                lib.database_config_set_sync_only(cfg, 1 if c.sync_only else 0)
                self._db = lib.database_create_with_config(path_b, cfg, err)
            finally:
                lib.database_config_destroy(cfg)

        if self._db == ffi.NULL:
            raise map_error(err[0], f"database_create failed for {self._path}")

        self._bridge = AsyncBridge()

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
            # Defensive: C contract is rc=-2 for not-found, but guard against
            # rc=0 + NULL in case of future changes.
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

    def batch_sync(self, ops: list[dict]) -> None:
        """Apply a list of put/del ops atomically in a single C batch call."""
        self._check_open()
        if not ops:
            return

        raw_ops = ffi.new(f"raw_op_t[{len(ops)}]")
        # Hold references to encoded buffers so they outlive the C call
        keep_alive = []
        for i, op in enumerate(ops):
            if "key" not in op:
                raise ValueError("op missing required 'key' field")
            t = op.get("type")
            if t not in ("put", "del"):
                raise ValueError(f"op type must be 'put' or 'del', got {t!r}")
            key_b = _normalize_key(op["key"], self._delimiter)
            key_buf = ffi.from_buffer(key_b)
            keep_alive.append(key_b)

            raw_ops[i].key = key_buf
            raw_ops[i].key_len = len(key_b)
            raw_ops[i].type = 0 if t == "put" else 1

            if t == "put":
                if "value" not in op:
                    raise ValueError("put op missing required 'value' field")
                val_b = _encode_value(op["value"])
                val_buf = ffi.from_buffer(val_b)
                keep_alive.append(val_b)
                raw_ops[i].value = val_buf
                raw_ops[i].value_len = len(val_b)
            else:
                raw_ops[i].value = ffi.NULL
                raw_ops[i].value_len = 0

        rc = lib.database_batch_sync_raw(
            self._db, self._delimiter_byte, raw_ops, len(ops),
        )
        if rc != 0:
            raise map_error(rc, "batch_sync failed")

    async def batch(self, ops: list[dict]) -> None:
        """Apply a list of put/del ops atomically via `database_batch_raw`.

        Async counterpart of `batch_sync`. Does not use `_dispatch_raw`
        because the C batch signature takes (delimiter, ops, count, promise)
        rather than the (key, len, delimiter, *extra, promise) shape the
        helper was built for.
        """
        self._check_open()
        if not ops:
            return

        raw_ops = ffi.new(f"raw_op_t[{len(ops)}]")
        # Hold references to encoded buffers so they outlive the C call
        keep_alive = []
        for i, op in enumerate(ops):
            if "key" not in op:
                raise ValueError("op missing required 'key' field")
            t = op.get("type")
            if t not in ("put", "del"):
                raise ValueError(f"op type must be 'put' or 'del', got {t!r}")
            key_b = _normalize_key(op["key"], self._delimiter)
            keep_alive.append(key_b)
            raw_ops[i].key = ffi.from_buffer(key_b)
            raw_ops[i].key_len = len(key_b)
            raw_ops[i].type = 0 if t == "put" else 1
            if t == "put":
                if "value" not in op:
                    raise ValueError("put op missing required 'value' field")
                val_b = _encode_value(op["value"])
                keep_alive.append(val_b)
                raw_ops[i].value = ffi.from_buffer(val_b)
                raw_ops[i].value_len = len(val_b)
            else:
                raw_ops[i].value = ffi.NULL
                raw_ops[i].value_len = 0

        # Batch doesn't fit _dispatch_raw's signature (no key_b prefix), so
        # inline the promise create/register/destroy pattern.
        fut, op_id, op_id_ptr = self._bridge.new_future("batch")
        self._bridge.register_pending(fut, op_id, op_id_ptr, "batch")
        promise = lib.promise_create(self._bridge.resolve_cb, self._bridge.reject_cb, op_id_ptr)
        if promise == ffi.NULL:
            raise WaveDBError("promise_create failed")
        try:
            rc = lib.database_batch_raw(
                self._db, self._delimiter_byte, raw_ops, len(ops), promise,
            )
            if rc != 0:
                raise map_error(rc, "async batch failed")
            await fut
            # database_batch_raw resolves with NULL; nothing to decode.
        finally:
            lib.promise_destroy(promise)

    # ---- public async API ----
    # `del` is a Python keyword, so the async delete method is named `delete`
    # (the sync counterpart is `del_sync`). All async methods dispatch C work
    # to the worker pool via promise_t and await the bridge future.

    async def put(self, key: "str | list[str]", value) -> None:
        """Async put. See `put_sync` for key/value semantics."""
        await self._async_put(_normalize_key(key, self._delimiter), _encode_value(value))

    async def get(self, key: "str | list[str]"):
        """Async get. Returns bytes or None if the key is absent."""
        return await self._async_get(_normalize_key(key, self._delimiter))

    async def delete(self, key: "str | list[str]") -> None:
        """Async delete. No-op if the key is absent."""
        await self._async_del(_normalize_key(key, self._delimiter))

    async def aclose(self) -> None:
        """Close the database, cancelling any in-flight async operations.

        NOTE: This does NOT await in-flight futures — it cancels them via
        close() → cancel_all_pending() → database_destroy(). C operations
        in flight when cancel_all_pending runs will still complete on the
        C work pool (database_destroy drains the pool), but their results
        are dropped. For get ops, the identifier_t* payload leaks in this
        case (accepted, matches the Dart binding's behavior). To drain
        without leaking, await each pending future before calling aclose.
        """
        self.close()

    async def __aenter__(self) -> "WaveDB":
        return self

    async def __aexit__(self, *exc) -> None:
        await self.aclose()

    # ---- private async ----

    def _dispatch_raw(
        self,
        c_fn,
        key_b: bytes,
        op_type: str,
        err_msg: str,
        *extra_args,
    ) -> tuple[asyncio.Future, Any]:
        """Create a promise, register the pending op, call the C async function.

        Returns (future, promise). Caller must promise_destroy in a finally
        block after awaiting the future.

        extra_args are passed between the delimiter and the promise in the
        C function call (e.g., value_b, len(value_b) for put).
        """
        self._check_open()
        fut, op_id, op_id_ptr = self._bridge.new_future(op_type)
        self._bridge.register_pending(fut, op_id, op_id_ptr, op_type)
        promise = lib.promise_create(self._bridge.resolve_cb, self._bridge.reject_cb, op_id_ptr)
        if promise == ffi.NULL:
            raise WaveDBError("promise_create failed")
        try:
            rc = c_fn(self._db, ffi.from_buffer(key_b), len(key_b), self._delimiter_byte, *extra_args, promise)
            if rc != 0:
                raise map_error(rc, err_msg)
        except BaseException:
            lib.promise_destroy(promise)
            raise
        return fut, promise

    def _decode_get_payload(self, payload) -> "bytes | None":
        """Decode the identifier_t* payload from database_get_raw.

        Payload contract (verified in src/Database/database.c):
          - `database_get_raw` enqueues `_raw_get_worker`, which calls
            `_database_get`. On a hit, `_database_get` calls
            `CONSUME(value)` then `promise_resolve(promise, consumed)`.
            `CONSUME` sets `yield=1` on the identifier's refcounter and
            returns the same pointer — so the payload is an
            `identifier_t*` with count=1, yield=1.
          - On a miss, `_database_get` resolves with NULL.
          - `database_put_raw` / `database_delete_raw` always resolve
            with NULL on the async path (sync_only mode is rejected
            up front; sync_only would malloc an int* instead, but that
            path is unreachable from the async API).

        To free the identifier correctly we must:
          1. REFERENCE (consumes the yield without incrementing count)
          2. identifier_destroy (decrements count to 0 and frees)
        Skipping the REFERENCE would leave yield=1; identifier_destroy
        would consume the yield and return without freeing → a leak.
        """
        if payload == ffi.NULL:
            return None
        ident = ffi.cast("identifier_t*", payload)
        try:
            len_out = ffi.new("size_t*")
            data_ptr = lib.identifier_get_data_copy(ident, len_out)
            if data_ptr == ffi.NULL:
                return None
            try:
                return ffi.buffer(data_ptr, len_out[0])[:]
            finally:
                lib.database_raw_value_free(data_ptr)
        finally:
            lib.refcounter_reference(ffi.cast("void*", ident))
            lib.identifier_destroy(ident)

    async def _async_put(self, key_b: bytes, value_b: bytes) -> None:
        fut, promise = self._dispatch_raw(
            lib.database_put_raw, key_b, "put", "async put failed",
            ffi.from_buffer(value_b), len(value_b),
        )
        try:
            await fut
            # database_put_raw resolves with NULL; nothing to decode.
        finally:
            lib.promise_destroy(promise)

    async def _async_get(self, key_b: bytes) -> "bytes | None":
        fut, promise = self._dispatch_raw(
            lib.database_get_raw, key_b, "get", "async get failed",
        )
        try:
            payload = await fut
            return self._decode_get_payload(payload)
        finally:
            lib.promise_destroy(promise)

    async def _async_del(self, key_b: bytes) -> None:
        # op_type is "del" to match database_delete_raw / del_sync naming.
        fut, promise = self._dispatch_raw(
            lib.database_delete_raw, key_b, "del", "async del failed",
        )
        try:
            await fut
            # database_delete_raw resolves with NULL; nothing to decode.
        finally:
            lib.promise_destroy(promise)

    # ---- lifecycle ----

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        if hasattr(self, "_bridge"):
            self._bridge.cancel_all_pending()
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
