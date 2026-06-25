"""cffi native library loader for libwavedb.

Tries the compiled out-of-line cffi extension (_native_ext) first for
direct C function calls (no runtime dispatch overhead). Falls back to
ABI mode (_native_abi) for development without a compiled extension.
"""
from __future__ import annotations

try:
    from ._native_ext import ffi, lib  # type: ignore[import-not-found]
except ImportError:
    from ._native_abi import ffi, lib  # type: ignore[no-redef]

# ASAN-safe free: use the main program's global symbol table so
# LD_PRELOAD=libasan.so routes free() through ASAN's interceptor.
# On Windows, dlopen(None) is unsupported for Python 3 (bpo-23606); load the
# Universal CRT so free() resolves through the same allocator the C core uses.
import os as _os
libc = ffi.dlopen("ucrtbase") if _os.name == "nt" else ffi.dlopen(None)