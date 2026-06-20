"""Sync and async iterators over database scans."""
from __future__ import annotations

import asyncio
from typing import AsyncIterator, Iterator

from ._native import ffi, lib


def _strip_chunk_padding(key_bytes: bytes, delimiter_byte: bytes) -> bytes:
    """Strip trailing null padding from each subscript segment.

    DEPRECATED: No longer called by scan_sync_raw. The C layer now stores
    exact byte lengths per subscript (path_meta with {chunk_count, byte_length})
    and reconstructs keys without null padding. Binary keys with real
    trailing nulls are preserved exactly.

    This function is retained for reference and could be used to clean up
    old on-disk data written before the C fix (which falls back to the
    legacy padded path). However, it would mangle binary keys with real
    trailing nulls, so use with caution.
    """
    if not key_bytes:
        return key_bytes
    parts = key_bytes.split(delimiter_byte)
    stripped = [p.rstrip(b"\x00") for p in parts]
    return delimiter_byte.join(stripped)


def scan_sync_raw(
    db_ptr,
    delimiter_byte: bytes,
    start: "bytes | None",
    end: "bytes | None",
) -> Iterator[tuple[str, bytes]]:
    """Yield (key_str, value_bytes) using database_scan_range_sync_raw.

    `delimiter_byte` is the single-byte delimiter as `bytes` (cffi ABI mode
    requires bytes, not int, for `char` parameters — using `ord()` here is
    the classic cffi bug).

    The C function allocates a `raw_result_t` array and returns it via out
    parameters. We free it in a `finally` block so the array is released
    whether the generator is exhausted, early-exited, or garbage-collected.
    """
    start_buf = ffi.from_buffer(start) if start is not None else ffi.NULL
    start_len = len(start) if start is not None else 0
    end_buf = ffi.from_buffer(end) if end is not None else ffi.NULL
    end_len = len(end) if end is not None else 0

    results = ffi.new("raw_result_t**")
    count = ffi.new("size_t*")
    rc = lib.database_scan_range_sync_raw(
        db_ptr,
        start_buf, start_len,
        end_buf, end_len,
        delimiter_byte,
        results, count,
    )
    if rc != 0:
        from ._errors import map_error
        raise map_error(rc, "scan_sync failed")

    try:
        for i in range(count[0]):
            r = results[0][i]
            key_raw = ffi.buffer(r.key, r.key_len)[:] if r.key != ffi.NULL else b""
            # No padding stripping needed: the C iterator now stores exact
            # byte lengths per subscript (path_meta) and reconstructs keys
            # without null padding. Binary keys with real trailing nulls
            # are preserved exactly. Old on-disk files without the 0x10 flag
            # fall back to the legacy padded path in the C layer; users with
            # such data should re-insert to get the fix.
            key = key_raw.decode("utf-8", errors="replace")
            value = (
                ffi.buffer(r.value, r.value_len)[:]
                if r.value != ffi.NULL
                else b""
            )
            yield key, value
    finally:
        lib.database_raw_results_free(results[0], count[0])


async def scan_async_iter(
    db_ptr,
    delimiter_byte: bytes,
    start: "bytes | None",
    end: "bytes | None",
) -> AsyncIterator[tuple[str, bytes]]:
    """Async iterator.

    Runs the blocking sync scan on a worker thread via
    `loop.run_in_executor`, then yields the collected results on the event
    loop thread. This is the simplest v1 approach; a true async scan using
    the C iterator API is a future enhancement.
    """
    loop = asyncio.get_running_loop()
    results = await loop.run_in_executor(
        None,
        _collect_scan,
        db_ptr,
        delimiter_byte,
        start,
        end,
    )
    for k, v in results:
        yield k, v


def _collect_scan(
    db_ptr,
    delimiter_byte: bytes,
    start: "bytes | None",
    end: "bytes | None",
) -> "list[tuple[str, bytes]]":
    return list(scan_sync_raw(db_ptr, delimiter_byte, start, end))
