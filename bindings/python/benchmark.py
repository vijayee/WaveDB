#!/usr/bin/env python3
"""WaveDB Python binding performance benchmarks.

Mirrors bindings/nodejs/benchmark.js structure: 5 sections × 4 WAL modes.
Reports ops/sec and us/op for cross-binding comparison.

Usage:
    WAVEDB_LIB_PATH=../../build-release/libwavedb.so python benchmark.py
    WAVEDB_LIB_PATH=../../build-release/libwavedb.so python benchmark.py --c-baseline 0.8
"""
from __future__ import annotations

import argparse
import asyncio
import os
import shutil
import sys
import tempfile
import time

from wavedb import WaveDB, WaveDBConfig

ITERATIONS = 5000
WARMUP = 100
BATCH_SIZE = 1000
FANOUTS = [1, 2, 4, 8, 16, 32]


def clean_path(path: str) -> None:
    if os.path.exists(path):
        shutil.rmtree(path)


def perf_ns() -> int:
    return time.perf_counter_ns()


def report(label: str, ops: int, elapsed_ns: int) -> None:
    elapsed_s = elapsed_ns / 1e9
    ops_per_sec = ops / elapsed_s if elapsed_s > 0 else 0
    us_per_op = elapsed_ns / ops / 1000 if ops > 0 else 0
    print(f"  {label:20s} {ops_per_sec:>10.0f} ops/sec  {us_per_op:>8.2f} us/op")


async def warmup_db(db: WaveDB) -> None:
    for i in range(WARMUP):
        await db.put(f"warmup{i}", f"value{i}")
        await db.get(f"warmup{i}")


async def bench_sequential_async(db: WaveDB) -> None:
    print("\nSequential Async:")
    print("-" * 50)
    t0 = perf_ns()
    for i in range(ITERATIONS):
        await db.put(f"key{i}", f"value{i}")
    report("put", ITERATIONS, perf_ns() - t0)

    t0 = perf_ns()
    for i in range(ITERATIONS):
        await db.get(f"key{i}")
    report("get", ITERATIONS, perf_ns() - t0)


def bench_sync(db: WaveDB) -> None:
    print("\nSync Operations:")
    print("-" * 50)
    t0 = perf_ns()
    for i in range(ITERATIONS):
        db.put_sync(f"sync{i}", f"value{i}")
    report("put_sync", ITERATIONS, perf_ns() - t0)

    t0 = perf_ns()
    for i in range(ITERATIONS):
        db.get_sync(f"sync{i}")
    report("get_sync", ITERATIONS, perf_ns() - t0)


async def bench_batch(db: WaveDB) -> None:
    print("\nBatch Operations:")
    print("-" * 50)
    ops = [{"type": "put", "key": f"batch{i}", "value": f"value{i}"} for i in range(BATCH_SIZE)]
    batches = ITERATIONS // BATCH_SIZE
    t0 = perf_ns()
    for _ in range(batches):
        await db.batch(ops)
    report(f"batch ({BATCH_SIZE}/batch)", ITERATIONS, perf_ns() - t0)


async def bench_concurrent(db: WaveDB) -> None:
    print("\nConcurrent Async:")
    print("-" * 50)
    cpu = os.cpu_count() or 4
    fanouts = [f for f in FANOUTS if f <= cpu * 2]
    if 1 not in fanouts:
        fanouts.insert(0, 1)

    for c in fanouts:
        keys = [f"cp{c}_{i}" for i in range(ITERATIONS)]
        # Concurrent puts (chunked by fanout)
        t0 = perf_ns()
        for idx in range(0, ITERATIONS, c):
            chunk = keys[idx:idx + c]
            await asyncio.gather(*[db.put(k, f"v{i}") for i, k in enumerate(chunk)])
        report(f"put (c={c})", ITERATIONS, perf_ns() - t0)

        # Concurrent gets
        t0 = perf_ns()
        for idx in range(0, ITERATIONS, c):
            chunk = keys[idx:idx + c]
            await asyncio.gather(*[db.get(k) for k in chunk])
        report(f"get (c={c})", ITERATIONS, perf_ns() - t0)


def bench_stream(db: WaveDB) -> None:
    print("\nStream Operations:")
    print("-" * 50)
    # NOTE: scan with 50+ keys triggers a pre-existing C-level segfault in
    # bnode_count during trie traversal (not related to path_meta changes).
    # Skip the stream benchmark until that crash is fixed. The segfault
    # can't be caught by Python exception handling.
    print("  stream: SKIPPED (pre-existing scan crash with 50+ keys)")


async def run_benchmarks(db_path: str, config: WaveDBConfig, label: str) -> None:
    clean_path(db_path)
    db = WaveDB(db_path, config=config)
    await warmup_db(db)

    await bench_sequential_async(db)
    bench_sync(db)
    await bench_batch(db)
    await bench_concurrent(db)
    bench_stream(db)

    await db.aclose()
    clean_path(db_path)


async def main() -> None:
    parser = argparse.ArgumentParser(description="WaveDB Python benchmarks")
    parser.add_argument("--c-baseline", type=float, default=None,
                        help="C microbench us/op for overhead factor comparison")
    args = parser.parse_args()

    print("WaveDB Performance Benchmarks (Python)")
    print("=" * 50)
    print(f"Python {sys.version.split()[0]}")
    print(f"Iterations: {ITERATIONS}")
    print(f"CPUs: {os.cpu_count()}")

    base = tempfile.mkdtemp(prefix="wavedb_bench_")

    # In-memory — use wal_sync_mode="none" (OS cache, no fsync) instead of
    # in_memory=True because async ops + in_memory=True is not yet supported
    # (C worker threads try to access WAL which doesn't exist in in-memory mode).
    # wal_sync_mode="none" gives close-to-in-memory performance with WAL enabled.
    print(f"\n{'=' * 50}")
    print("MODE: In-Memory (wal_sync_mode=none, OS cache)")
    print("=" * 50)
    await run_benchmarks(f"{base}/mem", WaveDBConfig(wal_sync_mode="none"), "in-memory")

    # Debounced WAL
    print(f"\n{'=' * 50}")
    print("MODE: DEBOUNCED WAL (100ms)")
    print("=" * 50)
    await run_benchmarks(
        f"{base}/debounced",
        WaveDBConfig(wal_sync_mode="debounced", wal_debounce_ms=100),
        "DEBOUNCED",
    )

    # Async WAL (OS cache, no fsync)
    print(f"\n{'=' * 50}")
    print("MODE: ASYNC WAL (OS cache, no fsync)")
    print("=" * 50)
    await run_benchmarks(
        f"{base}/async",
        WaveDBConfig(wal_sync_mode="none"),
        "ASYNC",
    )

    # Immediate WAL (fsync per write)
    print(f"\n{'=' * 50}")
    print("MODE: IMMEDIATE WAL (fsync per write)")
    print("=" * 50)
    await run_benchmarks(
        f"{base}/immediate",
        WaveDBConfig(wal_sync_mode="immediate"),
        "IMMEDIATE",
    )

    if args.c_baseline:
        print(f"\n{'=' * 50}")
        print(f"C baseline: {args.c_baseline} us/op")
        print("(Compare against sync put_sync us/op above for overhead factor)")

    shutil.rmtree(base)
    print(f"\n{'=' * 50}")
    print("Done.")


if __name__ == "__main__":
    asyncio.run(main())