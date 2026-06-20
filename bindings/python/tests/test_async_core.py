import asyncio
import pytest
from wavedb import WaveDB


@pytest.mark.asyncio
async def test_async_put_get(db_path):
    db = WaveDB(str(db_path))
    await db.put("users/alice", "Alice")
    val = await db.get("users/alice")
    assert val == b"Alice"
    await db.aclose()


@pytest.mark.asyncio
async def test_async_get_missing(db_path):
    db = WaveDB(str(db_path))
    assert await db.get("nope") is None
    await db.aclose()


@pytest.mark.asyncio
async def test_async_del(db_path):
    db = WaveDB(str(db_path))
    await db.put("k", "v")
    # `delete` (not `del`) because `del` is a Python keyword and cannot be
    # used as a method name. The sync API mirrors this with `del_sync`.
    await db.delete("k")
    assert await db.get("k") is None
    await db.aclose()


@pytest.mark.asyncio
async def test_concurrent_async_puts(db_path):
    db = WaveDB(str(db_path))
    await asyncio.gather(*[db.put(f"k{i}", str(i)) for i in range(100)])
    vals = await asyncio.gather(*[db.get(f"k{i}") for i in range(100)])
    assert [int(v) for v in vals] == list(range(100))
    await db.aclose()
