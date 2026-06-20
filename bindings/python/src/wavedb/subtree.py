"""Subtree: scoped view of the database under a prefix."""
from __future__ import annotations

from ._async import AsyncBridge, decode_identifier_payload
from ._errors import map_error
from ._native import ffi, lib
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


def _c_string(b: bytes) -> bytes:
    """Append a NUL terminator for C functions that take strlen-based prefixes.

    `database_subtree_open` expects a null-terminated C string (no
    prefix_len parameter). Appending b"\\x00" makes any NUL-free bytes
    object a valid C string.
    """
    return b + b"\x00"


# Return codes from the C sync raw API. The C layer uses:
#   0  -> success
#  -1  -> validation / argument error
#  -2  -> not found (database_subtree_get_sync_raw returns -2 when no value)
_GET_NOT_FOUND_RC = -2


class Subtree:
    """Scoped view into a WaveDB database under a fixed prefix.

    All keys passed to put/get/delete are automatically prefixed with
    `prefix{delimiter}` before being written to the underlying database.
    The subtree shares its parent WaveDB's AsyncBridge.
    """

    def __init__(
        self,
        db_ptr,
        prefix_b: bytes,
        delimiter_byte: bytes,
        delimiter: str,
        bridge: AsyncBridge,
    ) -> None:
        self._delimiter = delimiter
        self._delimiter_byte = delimiter_byte
        self._closed = False
        self._bridge = bridge
        # database_subtree_open takes a null-terminated prefix (no prefix_len).
        # prefix_b is a UTF-8 bytes string with no embedded null bytes, so
        # ffi.from_buffer yields a NUL-terminated C string when the bytes
        # object is NUL-free. _c_string appends an explicit NUL to be safe
        # regardless of how the caller built prefix_b.
        prefix_z = _c_string(prefix_b)
        self._st = lib.database_subtree_open(
            db_ptr, ffi.from_buffer(prefix_z), delimiter_byte,
        )
        if self._st == ffi.NULL:
            raise map_error(0, "subtree_open failed")

    def _check_open(self) -> None:
        if self._closed or self._st == ffi.NULL:
            raise WaveDBError("DATABASE_CLOSED: subtree has been closed")

    # ---- sync ----

    def put_sync(self, key, value) -> None:
        self._check_open()
        k = _normalize_key(key, self._delimiter)
        v = _encode_value(value)
        rc = lib.database_subtree_put_sync_raw(
            self._st, ffi.from_buffer(k), len(k), self._delimiter_byte,
            ffi.from_buffer(v), len(v),
        )
        if rc != 0:
            raise map_error(rc, "subtree put_sync failed")

    def get_sync(self, key):
        self._check_open()
        k = _normalize_key(key, self._delimiter)
        v_out = ffi.new("uint8_t**")
        len_out = ffi.new("size_t*")
        rc = lib.database_subtree_get_sync_raw(
            self._st, ffi.from_buffer(k), len(k), self._delimiter_byte,
            v_out, len_out,
        )
        if rc == _GET_NOT_FOUND_RC:
            return None
        if rc == 0 and v_out[0] == ffi.NULL:
            return None
        if rc != 0:
            raise map_error(rc, "subtree get_sync failed")
        try:
            return ffi.buffer(v_out[0], len_out[0])[:]
        finally:
            lib.database_raw_value_free(v_out[0])

    def del_sync(self, key) -> None:
        """Delete the value at `key` within the subtree.

        Deleting a non-existent key is a no-op (no exception raised).
        """
        self._check_open()
        k = _normalize_key(key, self._delimiter)
        rc = lib.database_subtree_delete_sync_raw(
            self._st, ffi.from_buffer(k), len(k), self._delimiter_byte,
        )
        if rc != 0 and rc != _GET_NOT_FOUND_RC:
            raise map_error(rc, "subtree del_sync failed")

    # ---- async ----
    # `del` is a Python keyword, so the async delete method is named `delete`
    # (the sync counterpart is `del_sync`). All async methods dispatch C work
    # to the worker pool via promise_t and await the bridge future.

    async def put(self, key, value) -> None:
        self._check_open()
        k = _normalize_key(key, self._delimiter)
        v = _encode_value(value)
        fut, promise = self._bridge.dispatch(
            lib.database_subtree_put_raw, self._st, k, self._delimiter_byte,
            "st_put", "subtree put failed",
            ffi.from_buffer(v), len(v),
        )
        try:
            await fut
            # database_subtree_put_raw resolves with NULL; nothing to decode.
        finally:
            lib.promise_destroy(promise)

    async def get(self, key):
        self._check_open()
        k = _normalize_key(key, self._delimiter)
        fut, promise = self._bridge.dispatch(
            lib.database_subtree_get_raw, self._st, k, self._delimiter_byte,
            "st_get", "subtree get failed",
        )
        try:
            payload = await fut
            return decode_identifier_payload(payload)
        finally:
            lib.promise_destroy(promise)

    async def delete(self, key) -> None:
        self._check_open()
        k = _normalize_key(key, self._delimiter)
        fut, promise = self._bridge.dispatch(
            lib.database_subtree_delete_raw, self._st, k, self._delimiter_byte,
            "st_del", "subtree del failed",
        )
        try:
            await fut
            # database_subtree_delete_raw resolves with NULL; nothing to decode.
        finally:
            lib.promise_destroy(promise)

    # ---- lifecycle ----

    def close(self) -> None:
        """Close the subtree handle.

        `database_subtree_close` decrements the subtree's refcounter and
        frees it when the count reaches zero. It does NOT drain in-flight
        async operations — subtree async ops (`put`/`get`/`delete`) are
        delegated to the parent database's work pool, and the C close call
        returns immediately without waiting for that pool.

        Behavior:
          - Safe to call while async ops are in flight on this subtree, as
            long as the parent `WaveDB` stays alive. The in-flight ops
            complete on the parent's work pool and their promises resolve
            (or reject) normally; their results are delivered to the
            awaiting futures.
          - If the parent `WaveDB` is closed (or `aclose`'d) while subtree
            ops are in flight, the parent's `database_destroy` drains the
            work pool, but in-flight futures are cancelled via
            `cancel_all_pending` and their payloads are dropped (matching
            `WaveDB.aclose` semantics).
          - To observe every in-flight result before close, `await` each
            pending future first.

        Closing twice is a no-op.
        """
        if self._closed:
            return
        self._closed = True
        if self._st != ffi.NULL:
            lib.database_subtree_close(self._st)
            self._st = ffi.NULL

    def __enter__(self) -> "Subtree":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass
