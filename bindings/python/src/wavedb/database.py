"""WaveDB database class."""
from __future__ import annotations

from typing import Any

from ._async import AsyncBridge, decode_identifier_payload
from ._errors import map_error
from ._native import ffi, lib
from .config import WaveDBConfig, WaveDBEncryption
from .exceptions import EncryptionError, InvalidPathError, WaveDBError


def _c_string(b: bytes) -> bytes:
    """Append a NUL terminator for C functions that take strlen-based prefixes.

    `database_subtree_open` and `database_subtree_delete_prefix` expect a
    null-terminated C string (no prefix_len parameter). Appending b"\\x00"
    makes any NUL-free bytes object a valid C string.
    """
    return b + b"\x00"


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

# Map user-facing encryption type strings to the C `encryption_type_t` enum
# values defined in src/Storage/encryption.h:
#   ENCRYPTION_NONE       = 0
#   ENCRYPTION_SYMMETRIC  = 1  (AES-256-GCM with a 32-byte user key)
#   ENCRYPTION_ASYMMETRIC = 2  (AES-256-GCM DEK wrapped by an EVP_PKEY)
#
# String keys are lowercase, hyphenated names a user would naturally write.
# "aes-256-gcm" is the canonical symmetric alias (matches the C header's
# description of SYMMETRIC mode). "asymmetric" covers the EVP_PKEY-wrapped
# DEK flow; we also accept "rsa-aes-256-gcm" as a descriptive alias.
_ENCRYPTION_TYPE_MAP = {
    "none": 0,
    "aes-256-gcm": 1,
    "symmetric": 1,
    "asymmetric": 2,
    "rsa-aes-256-gcm": 2,
}

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
        c = config or WaveDBConfig()
        # When in_memory=True, pass NULL location to C for truly ephemeral mode
        # (no WAL, no page file, no config save — data lost on close).
        if c.in_memory:
            path_b = ffi.NULL
        else:
            path_b = self._path.encode("utf-8")

        if encryption is not None:
            cfg = lib.encrypted_database_config_default()
            try:
                type_code = _ENCRYPTION_TYPE_MAP.get(encryption.type)
                if type_code is None:
                    raise EncryptionError(
                        f"unsupported encryption type: {encryption.type!r}"
                    )
                lib.encrypted_database_config_set_type(cfg, type_code)
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
        # Open child handles (Subtree, GraphLayer, GraphQLLayer) register
        # here so close() can refuse to run database_destroy while they
        # still hold subtree references — destroying the parent database
        # first would leave dangling pointers (UAF).
        self._open_subtrees: set = set()

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

    def _build_raw_ops(self, ops: list[dict]) -> "tuple[Any, list[bytes]]":
        """Build a raw_op_t array from a list of op dicts.

        Returns (raw_ops_cdata, keep_alive_list). The keep_alive list holds
        references to the encoded key/value bytes so they outlive the C call.
        """
        raw_ops = ffi.new(f"raw_op_t[{len(ops)}]")
        keep_alive: list[bytes] = []
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
        return raw_ops, keep_alive

    def batch_sync(self, ops: list[dict]) -> None:
        """Apply a list of put/del ops atomically in a single C batch call."""
        self._check_open()
        if not ops:
            return
        raw_ops, keep_alive = self._build_raw_ops(ops)
        rc = lib.database_batch_sync_raw(
            self._db, self._delimiter_byte, raw_ops, len(ops),
        )
        if rc != 0:
            raise map_error(rc, "batch_sync failed")

    # ---- read stream / iterator ----

    def create_read_stream(
        self,
        *,
        start: "str | None" = None,
        end: "str | None" = None,
        limit: "int | None" = None,
    ):
        """Synchronous generator yielding (key_str, value_bytes) tuples.

        Scans the key range [`start`, `end`] using
        `database_scan_range_sync_raw`. If `limit` is given, stops after
        yielding that many pairs.
        """
        from .iterator import scan_sync_raw

        start_b = start.encode("utf-8") if start is not None else None
        end_b = end.encode("utf-8") if end is not None else None
        count = 0
        for kv in scan_sync_raw(self._db, self._delimiter_byte, start_b, end_b):
            yield kv
            count += 1
            if limit is not None and count >= limit:
                return

    def create_read_stream_async(
        self,
        *,
        start: "str | None" = None,
        end: "str | None" = None,
        limit: "int | None" = None,
    ):
        """Async iterator yielding (key_str, value_bytes) tuples.

        The blocking C scan runs on a worker thread via
        `loop.run_in_executor`; results are then yielded on the event loop
        thread. If `limit` is given, stops after yielding that many pairs.
        """
        from .iterator import scan_async_iter

        start_b = start.encode("utf-8") if start is not None else None
        end_b = end.encode("utf-8") if end is not None else None
        return _limited_async_iter(
            scan_async_iter(self._db, self._delimiter_byte, start_b, end_b),
            limit,
        )

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
        raw_ops, keep_alive = self._build_raw_ops(ops)
        # Batch doesn't fit _dispatch_raw's (db, key, len, delim, *extra, promise)
        # signature, so inline the promise create/register/destroy pattern.
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

    def put_object_sync(self, key: "str | list[str]", obj: dict) -> None:
        """Flatten `obj` into path/value pairs and write atomically via batch."""
        from .object_ops import flatten_object
        self.batch_sync(flatten_object(key, obj, self._delimiter))

    def get_object_sync(self, key: "str | list[str]") -> dict:
        """Scan everything under `key` and reconstruct the nested dict."""
        from .object_ops import reconstruct_object
        from .iterator import scan_sync_raw
        prefix_b = _normalize_key(key, self._delimiter)
        kvs = list(scan_sync_raw(self._db, self._delimiter_byte, prefix_b, None))
        return reconstruct_object(key, kvs, self._delimiter)

    async def put_object(self, key: "str | list[str]", obj: dict) -> None:
        """Async counterpart of `put_object_sync`."""
        from .object_ops import flatten_object
        await self.batch(flatten_object(key, obj, self._delimiter))

    async def get_object(self, key: "str | list[str]") -> dict:
        """Async counterpart of `get_object_sync`."""
        from .object_ops import reconstruct_object
        from .iterator import scan_async_iter
        prefix_b = _normalize_key(key, self._delimiter)
        kvs = [kv async for kv in scan_async_iter(self._db, self._delimiter_byte, prefix_b, None)]
        return reconstruct_object(key, kvs, self._delimiter)

    # ---- subtree ----

    def open_subtree(self, prefix, delimiter: "str | None" = None) -> "Subtree":
        """Open a scoped view of the database under `prefix`.

        Keys passed to the returned Subtree are automatically prefixed with
        `prefix{delimiter}`. The subtree shares this database's AsyncBridge.
        Closing the parent database cancels in-flight subtree operations
        (subtree async ops are dispatched on the parent's work pool).
        """
        from .subtree import Subtree

        self._check_open()
        delim = delimiter if delimiter is not None else self._delimiter
        delim_bytes = delim.encode("utf-8")
        if len(delim_bytes) != 1:
            raise InvalidPathError("delimiter must encode to a single byte")
        prefix_b = _normalize_key(prefix, delim)
        st = Subtree(self._db, prefix_b, delim_bytes, delim, self._bridge, self)
        self._open_subtrees.add(st)
        return st

    def _unregister_subtree(self, st) -> None:
        """Called by Subtree/GraphLayer/GraphQLLayer.close() to drop itself
        from the set of open child handles. Discard is idempotent so a
        double-close is safe."""
        self._open_subtrees.discard(st)

    def delete_subtree(self, prefix, delimiter: "str | None" = None) -> None:
        """Delete all keys matching `prefix{delimiter}*` from the database."""
        self._check_open()
        delim = delimiter if delimiter is not None else self._delimiter
        delim_bytes = delim.encode("utf-8")
        if len(delim_bytes) != 1:
            raise InvalidPathError("delimiter must encode to a single byte")
        prefix_b = _normalize_key(prefix, delim)
        # database_subtree_delete_prefix takes a null-terminated prefix (no
        # prefix_len). _c_string appends the NUL that makes the buffer a
        # valid C string.
        prefix_z = _c_string(prefix_b)
        rc = lib.database_subtree_delete_prefix(
            self._db, ffi.from_buffer(prefix_z), delim_bytes,
        )
        if rc != 0:
            raise map_error(rc, "delete_subtree failed")

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

        Thin wrapper over `AsyncBridge.dispatch` that passes the database
        handle and delimiter. Returns (future, promise); on error the
        promise is destroyed and a mapped exception is raised. The caller
        must `promise_destroy` in a finally block after awaiting.
        """
        self._check_open()
        return self._bridge.dispatch(
            c_fn, self._db, key_b, self._delimiter_byte,
            op_type, err_msg, *extra_args,
        )

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
            return decode_identifier_payload(payload)
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
        if self._open_subtrees:
            raise WaveDBError(
                f"DATABASE_CLOSED: cannot close while "
                f"{len(self._open_subtrees)} subtree(s) are still open"
            )
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


async def _limited_async_iter(it, limit: "int | None"):
    """Wrap an async iterator, stopping after `limit` items if set."""
    count = 0
    async for kv in it:
        yield kv
        count += 1
        if limit is not None and count >= limit:
            return
