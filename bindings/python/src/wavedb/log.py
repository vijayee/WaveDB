"""WaveDB logging and progress callbacks.

Wraps the rxi log infrastructure (src/Util/log.c) exposed to FFI by
src/Util/log_bridge.c. The C bridge formats the rxi ``log_Event``'s ``va_list``
with ``vsnprintf`` and invokes a Python-friendly callback
``(level:int, message:str, udata)`` -- the binding never touches ``log_Event``
or ``va_list`` (cffi ABI mode has no C compiler, and ``va_list`` is not safely
reachable from cffi anyway).

Lifecycle / re-registration safety
----------------------------------
rxi's ``log_add_callback`` appends into a fixed ``MAX_CALLBACKS=32`` array and
has **no remove**. Two consequences, both handled here:

* Every cffi callback we hand to rxi must stay alive for the process lifetime,
  or rxi will eventually call a freed function (use-after-free). All callbacks
  are kept in the module-level ``_kept_cbs`` list (never emptied).
* Re-registering a handler does NOT unregister the old one. We disarm prior
  slots (``_slots``) so only the LATEST registered handler fires -- no
  double-fire -- while the old cffi callbacks stay alive (safe but dormant).

The handler runs synchronously on the thread that emitted the log event (the
calling thread of whatever WaveDB API is in progress). A handler MUST NOT call
back into WaveDB (reentrancy). Keep it short.
"""
from __future__ import annotations

from enum import IntEnum

from ._native import ffi, lib
from .exceptions import WaveDBError


class LogLevel(IntEnum):
    """rxi log levels (src/Util/log.h: LOG_LEVEL_*)."""

    TRACE = 0
    DEBUG = 1
    INFO = 2
    WARN = 3
    ERROR = 4
    FATAL = 5


# Module-level constants mirroring the enum, for callers who prefer bare ints.
LOG_LEVEL_TRACE = 0
LOG_LEVEL_DEBUG = 1
LOG_LEVEL_INFO = 2
LOG_LEVEL_WARN = 3
LOG_LEVEL_ERROR = 4
LOG_LEVEL_FATAL = 5

# Module-level state. `_kept_cbs` holds every cffi callback ever registered
# (immortal -- rxi has no remove, so a freed cb is a UAF). `_slots` records the
# disarm flag for each registration so only the latest fires. Both grow for the
# process lifetime; this is by design (rxi's no-remove constraint).
_kept_cbs: list = []   # immortal cffi callbacks (never freed)
_slots: list = []       # [{"armed": bool, "handler": callable}]


def set_log_callback(handler, level: int = LOG_LEVEL_INFO) -> None:
    """Register ``handler(level:int, message:str)`` for log events.

    Replaces any prior registration: previous slots are disarmed so only the
    latest handler fires (no double-fire). The prior cffi callbacks stay alive
    (rxi has no remove), which is safe -- they simply do nothing once disarmed.

    Fires synchronously on the calling thread for every event at or above
    ``level``. The handler MUST NOT call back into WaveDB (reentrancy). This is
    a process-lifetime registration (rxi has no callback remove).
    """
    if not callable(handler):
        raise TypeError(f"handler must be callable, got {type(handler).__name__}")
    # Disarm all prior slots so only this new one fires.
    for prev in _slots:
        prev["armed"] = False
    slot = {"armed": True, "handler": handler}
    _slots.append(slot)

    @ffi.callback("void(int, const char*, void*)")
    def _wrapper(lvl, msg, _udata):
        if not slot["armed"]:
            return
        try:
            text = ffi.string(msg).decode("utf-8", "replace")
            handler(int(lvl), text)
        except Exception:
            # A logging callback must never raise into C -- swallow to avoid
            # aborting the in-progress WaveDB API call.
            pass

    _kept_cbs.append(_wrapper)  # immortalize (no UAF even though rxi never removes)
    rc = lib.wavedb_log_add_callback(_wrapper, ffi.NULL, int(level))
    if rc != 0:
        # rxi's callback array is full (MAX_CALLBACKS=32, no remove). The new
        # handler will never fire; surface this rather than silently dropping.
        raise WaveDBError(
            "log callback registry full (rxi MAX_CALLBACKS=32, no remove); "
            "too many set_log_callback/set_progress_handler calls this process"
        )


def set_progress_handler(handler, level: int = LOG_LEVEL_INFO) -> None:
    """Register ``handler(phase:str, current:int, total:int)`` for progress.

    Filters log events down to the uniform vector-layer progress format::

        vector\\t{phase}\\t{current}\\t{total}

    (phases: ``scan`` / ``delete`` / ``train`` / ``rebuild`` / ``done``),
    emitted per 8000-op chunk by ``VectorLayer.migrate()`` (and by direct
    ``train()``/``rebuild()`` calls). Malformed lines are silently ignored.

    Same lifecycle / re-registration contract as :func:`set_log_callback`
    (disarms prior handlers; cffi cbs stay alive).
    """
    if not callable(handler):
        raise TypeError(f"handler must be callable, got {type(handler).__name__}")

    def _progress_filter(lvl: int, message: str) -> None:
        parts = message.split("\t")
        if len(parts) != 4 or parts[0] != "vector":
            return
        try:
            phase = parts[1]
            current = int(parts[2])
            total = int(parts[3])
        except ValueError:
            return
        handler(phase, current, total)

    set_log_callback(_progress_filter, level)


def set_quiet(enable: bool = True) -> None:
    """Suppress rxi's default stderr log output.

    Registered callbacks still fire regardless of quiet (the two paths are
    independent in rxi). Use this to silence stderr while still collecting
    progress via a handler.
    """
    lib.log_set_quiet(1 if enable else 0)


def set_level(level: int) -> None:
    """Set the minimum level for rxi's default stderr output."""
    lib.log_set_level(int(level))