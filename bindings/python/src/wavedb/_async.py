"""Bridge between C promise_t callbacks and Python asyncio futures.

The C library invokes promise callbacks on worker threads. We must not touch
asyncio.Future from a foreign thread. Instead, the cffi callback schedules
_deliver on the future's loop via loop.call_soon_threadsafe.
"""
from __future__ import annotations

import asyncio
import itertools
import logging
from dataclasses import dataclass
from typing import Any

from ._errors import map_error
from ._native import ffi, lib
from .exceptions import WaveDBError


_log = logging.getLogger("wavedb.async")


def decode_identifier_payload(payload) -> "bytes | None":
    """Decode the identifier_t* payload from database_get_raw /
    database_subtree_get_raw.

    Payload contract (verified against src/Database/database.c):
      - Hit: identifier_t* with count=1, yield=1 (CONSUME'd by _database_get
        before promise_resolve).
      - Miss: NULL.

    Decoding sequence:
      1. identifier_get_data_copy -> malloc'd uint8_t* (freed with
         database_raw_value_free).
      2. Copy to Python bytes.
      3. refcounter_reference -> consumes the yield without incrementing
         count.
      4. identifier_destroy -> decrements count to 0 and frees the
         identifier.

    REFERENCE is mandatory before destroy: identifier_destroy calls
    refcounter_dereference which consumes the yield first — without
    REFERENCE, the identifier would be leaked.
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


@dataclass
class PendingOp:
    op_id: int
    future: asyncio.Future
    loop: asyncio.AbstractEventLoop
    op_type: str
    op_id_ptr: Any  # cffi-allocated intptr_t*; bridge owns lifetime via _all_op_id_ptrs
    cancelled: bool = False


class AsyncBridge:
    """Per-database bridge. One instance per WaveDB connection."""

    # Class-level: op-ids grow monotonically across connections; only meaningful
    # within a bridge's _pending dict.
    _id_counter = itertools.count(1)

    def __init__(self) -> None:
        self._pending: dict[int, PendingOp] = {}
        # Reverse index for O(1) unregister by future.
        self._future_to_op_id: dict[asyncio.Future, int] = {}
        # The bridge owns op_id_ptr lifetime for safety across any calling
        # pattern. Each entry is 8 bytes; the high-water mark of concurrent ops
        # bounds the count. Never cleared — freed when the bridge is GC'd
        # (after database_destroy has drained the work pool and all promises
        # are destroyed).
        self._all_op_id_ptrs: list[Any] = []
        # Shared cffi callbacks (kept alive at instance scope; never GC'd while
        # C holds a pointer).
        self._resolve_cb = ffi.callback("void(void*, void*)")(self._on_resolve)
        self._reject_cb = ffi.callback("void(void*, async_error_t*)")(self._on_reject)

    # ---- public API ----

    def new_future(self, op_type: str = "unknown") -> tuple[asyncio.Future, int, Any]:
        """Create a future + op-id pair. Returns (future, op_id, op_id_ptr).

        Caller passes op_id_ptr as promise_create's ctx. Caller must call
        register_pending() with the same op_id before invoking the C async fn.
        """
        loop = asyncio.get_running_loop()
        fut: asyncio.Future = loop.create_future()
        op_id = next(self._id_counter)
        op_id_ptr = ffi.new("intptr_t*", op_id)
        self._all_op_id_ptrs.append(op_id_ptr)  # bridge owns the lifetime
        return fut, op_id, op_id_ptr

    def register_pending(self, fut: asyncio.Future, op_id: int, op_id_ptr: Any, op_type: str) -> None:
        loop = fut.get_loop()
        self._pending[op_id] = PendingOp(
            op_id=op_id, future=fut, loop=loop, op_type=op_type, op_id_ptr=op_id_ptr,
        )
        self._future_to_op_id[fut] = op_id

    def dispatch(
        self,
        c_fn,
        handle_ptr: Any,
        key_b: bytes,
        delimiter_byte: bytes,
        op_type: str,
        err_msg: str,
        *extra_args,
    ) -> tuple[asyncio.Future, Any]:
        """Create a promise, register the pending op, call the C async function.

        `handle_ptr` is the C handle (database_t* or database_subtree_t*) that
        the C function is invoked on. `extra_args` are passed between the
        delimiter and the promise in the C function call (e.g.,
        value_b, len(value_b) for put).

        Returns (fut, promise). On error the promise is destroyed and the
        mapped exception is raised. On success the caller must promise_destroy
        in a finally block after awaiting the future.
        """
        fut, op_id, op_id_ptr = self.new_future(op_type)
        self.register_pending(fut, op_id, op_id_ptr, op_type)
        promise = lib.promise_create(self._resolve_cb, self._reject_cb, op_id_ptr)
        if promise == ffi.NULL:
            raise WaveDBError("promise_create failed")
        try:
            rc = c_fn(
                handle_ptr, ffi.from_buffer(key_b), len(key_b),
                delimiter_byte, *extra_args, promise,
            )
            if rc != 0:
                raise map_error(rc, err_msg)
        except BaseException:
            lib.promise_destroy(promise)
            raise
        return fut, promise

    @property
    def resolve_cb(self):
        return self._resolve_cb

    @property
    def reject_cb(self):
        return self._reject_cb

    def cancel_all_pending(self) -> None:
        for op in list(self._pending.values()):
            op.cancelled = True
            if not op.future.done():
                try:
                    op.future.cancel()
                except (RuntimeError, asyncio.InvalidStateError):
                    pass
        self._pending.clear()
        self._future_to_op_id.clear()

    # ---- test helpers (also usable for non-C scheduling) ----

    def schedule_resolve(self, fut: asyncio.Future, payload: Any) -> None:
        """Schedule a resolve on the future's loop. Used by tests."""
        loop = fut.get_loop()
        try:
            loop.call_soon_threadsafe(self._deliver, fut, payload, False)
        except RuntimeError:
            # Loop closed between scheduling and delivery — drop silently.
            _log.warning("schedule_resolve on closed loop; dropping result")

    def schedule_reject(self, fut: asyncio.Future, exception: BaseException) -> None:
        """Schedule a rejection on the future's loop. Used by tests."""
        loop = fut.get_loop()
        try:
            loop.call_soon_threadsafe(self._set_exception, fut, exception)
        except RuntimeError:
            _log.warning("schedule_reject on closed loop; dropping exception")

    # ---- internal: cffi callbacks (run on C worker threads) ----

    def _on_resolve(self, ctx: Any, payload: Any) -> None:
        # Runs on C worker thread. GIL is held (cffi callback acquires it).
        if ctx == ffi.NULL:
            return
        op_id_ptr = ffi.cast("intptr_t*", ctx)
        op_id = int(op_id_ptr[0])
        op = self._pending.get(op_id)
        if op is None:
            return
        try:
            op.loop.call_soon_threadsafe(self._deliver, op.future, payload, False)
        except RuntimeError:
            _log.warning("_on_resolve on closed loop; dropping result for op %d", op_id)

    def _on_reject(self, ctx: Any, error: Any) -> None:
        if ctx == ffi.NULL:
            if error != ffi.NULL:
                lib.error_destroy(error)
            return
        op_id_ptr = ffi.cast("intptr_t*", ctx)
        op_id = int(op_id_ptr[0])
        op = self._pending.get(op_id)
        # Extract message and destroy the error object now (it's refcounted by C
        # but we don't keep a reference once we've read the string).
        msg = ""
        if error != ffi.NULL:
            cmsg = lib.error_get_message(error)
            if cmsg != ffi.NULL:
                msg = ffi.string(cmsg).decode("utf-8", errors="replace")
            # TODO: async_error_t also carries file/function/line (see
            # src/Workers/error.h), but no C accessors (error_get_file,
            # error_get_function, error_get_line) are exposed, and the struct
            # is opaque in our cdef. Enriching the message with source location
            # would require adding accessors — deferred until the C API exposes
            # them.
            lib.error_destroy(error)
        if op is None:
            return
        try:
            op.loop.call_soon_threadsafe(self._deliver_error, op.future, msg)
        except RuntimeError:
            _log.warning("_on_reject on closed loop; dropping error for op %d", op_id)

    # ---- internal: loop-thread delivery ----

    def _deliver(self, fut: asyncio.Future, payload: Any, loop_closed: bool = False) -> None:
        # Runs on the loop thread (or directly in tests with loop_closed=True).
        # loop_closed is a test-only escape hatch; in production,
        # call_soon_threadsafe raises RuntimeError on a closed loop before
        # _deliver is invoked.
        if loop_closed or fut.done():
            # Cancelled/closed before delivery. For get ops the payload is an
            # identifier_t* with yield=1; dropping it leaks (accepted, matches
            # Dart). To fix, dispatch on op_type from self._future_to_op_id and
            # free the payload appropriately.
            # TODO(task-followup): free cancelled get payloads via identifier_destroy.
            return
        fut.set_result(payload)
        self._unregister(fut)

    def _deliver_error(self, fut: asyncio.Future, message: str) -> None:
        if fut.done():
            return
        fut.set_exception(map_error(0, message))
        self._unregister(fut)

    def _set_exception(self, fut: asyncio.Future, exception: BaseException) -> None:
        """Used by schedule_reject (test path) to set an arbitrary exception."""
        if fut.done():
            return
        fut.set_exception(exception)
        self._unregister(fut)

    def _unregister(self, fut: asyncio.Future) -> None:
        op_id = self._future_to_op_id.pop(fut, None)
        if op_id is not None:
            del self._pending[op_id]
        # Do NOT free op_id_ptr — the bridge owns it via _all_op_id_ptrs
        # (see I-1) and keeps it alive until database_destroy has drained.
