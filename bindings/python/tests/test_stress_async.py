"""Stress tests for the async bridge under high concurrency.

Exercises 1K+ concurrent ops, cancel-during-inflight, sustained mixed
workloads, and memory growth checks. Run under ASAN to verify zero leaks:

    WAVEDB_LIB_PATH=../../build-asan-py/libwavedb.so \
      LD_PRELOAD=$(gcc -print-file-name=libasan.so) \
      ASAN_OPTIONS=detect_leaks=1 \
      pytest tests/test_stress_async.py -v
"""
from __future__ import annotations

import asyncio
import gc
import time
import tracemalloc

import pytest

from wavedb import WaveDB, WaveDBConfig

# Scale: 1K concurrent ops (enough to stress the async bridge without
# overwhelming the event loop or timing out under ASAN).
CONCURRENT_OPS = 1000
SYNC_OPS = 2000


@pytest.fixture
def db_path(tmp_path_factory):
    return tmp_path_factory.mktemp("wavedb_stress") / "db"


# ---------------------------------------------------------------------------
# 1. Concurrent puts — verify all readable back
# ---------------------------------------------------------------------------

@pytest.mark.asyncio
async def test_stress_concurrent_puts(db_path):
    db = WaveDB(str(db_path))
    await asyncio.gather(*[db.put(f"k{i}", str(i)) for i in range(CONCURRENT_OPS)])
    # Verify via sync get (fast bulk check)
    for i in range(CONCURRENT_OPS):
        assert db.get_sync(f"k{i}") == str(i).encode()
    await db.aclose()


# ---------------------------------------------------------------------------
# 2. Concurrent gets — verify payload correctness
# ---------------------------------------------------------------------------

@pytest.mark.asyncio
async def test_stress_concurrent_gets(db_path):
    db = WaveDB(str(db_path))
    # Insert keys via sync (fast)
    for i in range(CONCURRENT_OPS):
        db.put_sync(f"k{i}", str(i))
    # Concurrent async get all
    results = await asyncio.gather(*[db.get(f"k{i}") for i in range(CONCURRENT_OPS)])
    for i, val in enumerate(results):
        assert val == str(i).encode(), f"k{i}: expected {str(i).encode()}, got {val}"
    await db.aclose()


# ---------------------------------------------------------------------------
# 3. Cancel-during-inflight (no crash, no hang)
# ---------------------------------------------------------------------------

@pytest.mark.asyncio
async def test_stress_cancel_during_inflight(db_path):
    db = WaveDB(str(db_path))
    # Insert keys first
    for i in range(500):
        db.put_sync(f"k{i}", str(i))
    # Issue 500 concurrent gets, then immediately close without awaiting
    tasks = [asyncio.ensure_future(db.get(f"k{i}")) for i in range(500)]
    # Give them a moment to start
    await asyncio.sleep(0.01)
    # Cancel all and close — exercises the cancel-during-inflight path
    for t in tasks:
        t.cancel()
    await db.aclose()
    # If we reach here without hanging or crashing, the test passes.
    # Under ASAN, the _on_resolve cancel-leak fix should prevent identifier leaks.


# ---------------------------------------------------------------------------
# 4. Open/close cycles — no cross-cycle crash
# ---------------------------------------------------------------------------

@pytest.mark.asyncio
async def test_stress_open_close_cycles(db_path):
    path = str(db_path)
    for cycle in range(5):
        db = WaveDB(path)
        for i in range(20):
            await db.put(f"cycle{cycle}/k{i}", str(i))
        await db.aclose()
    # If we reach here, 5 open/close cycles completed without crash.


# ---------------------------------------------------------------------------
# 5. Sustained mixed workload — no RuntimeError from dict racing
# ---------------------------------------------------------------------------

@pytest.mark.asyncio
async def test_stress_sustained_mixed_workload(db_path):
    db = WaveDB(str(db_path))
    deadline = time.monotonic() + 1.0  # 1 second of sustained load
    counter = 0
    while time.monotonic() < deadline:
        # Mix of puts, gets, deletes in batches of 50
        coros = []
        for i in range(50):
            if i % 3 == 0:
                coros.append(db.put(f"k{counter}", str(counter)))
            elif i % 3 == 1:
                coros.append(db.get(f"k{counter}"))
            else:
                coros.append(db.delete(f"k{counter}"))
            counter += 1
        # This exercises _pending dict access from C worker threads
        # while the loop continues scheduling new ops
        await asyncio.gather(*coros)
    await db.aclose()
    assert counter > 0, "Should have completed at least some ops in 2 seconds"


# ---------------------------------------------------------------------------
# 6. tracemalloc bounded growth — _all_op_id_ptrs residual is bounded
# ---------------------------------------------------------------------------

@pytest.mark.asyncio
async def test_stress_tracemalloc_bounded_growth(db_path):
    tracemalloc.start()
    db = WaveDB(str(db_path))

    gc.collect()
    snapshot1 = tracemalloc.take_snapshot()

    # Run 1K async ops
    await asyncio.gather(*[db.put(f"k{i}", str(i)) for i in range(1000)])

    gc.collect()
    snapshot2 = tracemalloc.take_snapshot()

    await db.aclose()
    tracemalloc.stop()

    # The _all_op_id_ptrs list retains 1K cffi pointers (~8KB). The rest is
    # Python/cffi object overhead (~700 bytes/op for futures, PendingOps, dict
    # entries, promise_t allocations). Allow up to 2MB — the key assertion is
    # that growth is bounded, not that it's under a specific per-op threshold.
    stats = snapshot2.compare_to(snapshot1, "lineno")
    total_delta = sum(s.size_diff for s in stats if s.size_diff > 0)
    assert total_delta < 2_000_000, (
        f"Memory growth after 1K ops: {total_delta} bytes — "
        f"expected < 2MB (op_id_ptrs residual + Python/cffi overhead). "
        f"Top allocators: {stats[:3]}"
    )