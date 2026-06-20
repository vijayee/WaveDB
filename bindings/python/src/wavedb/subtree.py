"""Subtree: scoped view of the database under a prefix."""
from __future__ import annotations

from typing import Any

from ._async import AsyncBridge
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
        # object is NUL-free. We append an explicit NUL to be safe regardless
        # of how the caller built prefix_b.
        prefix_z = prefix_b + b"\x00"
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
        fut, promise = self._dispatch(
            lib.database_subtree_put_raw, k, "st_put", "subtree put failed",
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
        fut, promise = self._dispatch(
            lib.database_subtree_get_raw, k, "st_get", "subtree get failed",
        )
        try:
            payload = await fut
            return self._decode_get_payload(payload)
        finally:
            lib.promise_destroy(promise)

    async def delete(self, key) -> None:
        self._check_open()
        k = _normalize_key(key, self._delimiter)
        fut, promise = self._dispatch(
            lib.database_subtree_delete_raw, k, "st_del", "subtree del failed",
        )
        try:
            await fut
            # database_subtree_delete_raw resolves with NULL; nothing to decode.
        finally:
            lib.promise_destroy(promise)

    # ---- private ----

    def _dispatch(
        self,
        c_fn,
        key_b: bytes,
        op_type: str,
        err_msg: str,
        *extra_args,
    ) -> tuple[Any, Any]:
        """Create a promise + register pending + call the C async function.

        Returns (fut, promise). Caller must promise_destroy in a finally
        block after awaiting the future.

        extra_args are passed between the delimiter and the promise in the
        C function call (e.g., value_b, len(value_b) for put).
        """
        fut, op_id, op_id_ptr = self._bridge.new_future(op_type)
        self._bridge.register_pending(fut, op_id, op_id_ptr, op_type)
        promise = lib.promise_create(
            self._bridge.resolve_cb, self._bridge.reject_cb, op_id_ptr,
        )
        if promise == ffi.NULL:
            raise WaveDBError("promise_create failed")
        try:
            rc = c_fn(
                self._st, ffi.from_buffer(key_b), len(key_b),
                self._delimiter_byte, *extra_args, promise,
            )
            if rc != 0:
                raise map_error(rc, err_msg)
        except BaseException:
            lib.promise_destroy(promise)
            raise
        return fut, promise

    def _decode_get_payload(self, payload):
        """Decode the identifier_t* payload from database_subtree_get_raw.

        Same contract as WaveDB._decode_get_payload (see that method for the
        CONSUME/REFERENCE/destroy rationale). Duplicated here to keep the
        Subtree self-contained; a future refactor could extract it to a
        shared helper if more consumers need it.
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

    # ---- lifecycle ----

    def close(self) -> None:
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