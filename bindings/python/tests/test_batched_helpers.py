"""Tests for batched async helpers (put_many, delete_many, get_many)."""
import pytest
from wavedb import WaveDB, WaveDBConfig


@pytest.mark.asyncio
async def test_put_many(db_path):
    db = WaveDB(str(db_path))
    items = [(f"k{i}", str(i)) for i in range(100)]
    await db.put_many(items)
    for i in range(100):
        assert db.get_sync(f"k{i}") == str(i).encode()
    await db.aclose()


@pytest.mark.asyncio
async def test_put_many_empty(db_path):
    db = WaveDB(str(db_path))
    await db.put_many([])  # no-op
    await db.aclose()


@pytest.mark.asyncio
async def test_delete_many(db_path):
    db = WaveDB(str(db_path))
    for i in range(50):
        db.put_sync(f"k{i}", str(i))
    await db.delete_many([f"k{i}" for i in range(50)])
    for i in range(50):
        assert db.get_sync(f"k{i}") is None
    await db.aclose()


@pytest.mark.asyncio
async def test_delete_many_empty(db_path):
    db = WaveDB(str(db_path))
    await db.delete_many([])  # no-op
    await db.aclose()


@pytest.mark.asyncio
async def test_get_many(db_path):
    db = WaveDB(str(db_path))
    for i in range(50):
        db.put_sync(f"k{i}", str(i))
    results = await db.get_many([f"k{i}" for i in range(50)])
    for i, val in enumerate(results):
        assert val == str(i).encode()
    await db.aclose()


@pytest.mark.asyncio
async def test_get_many_empty(db_path):
    db = WaveDB(str(db_path))
    results = await db.get_many([])
    assert results == []
    await db.aclose()


@pytest.mark.asyncio
async def test_get_many_missing_keys(db_path):
    db = WaveDB(str(db_path))
    db.put_sync("k0", "v0")
    results = await db.get_many(["k0", "missing", "k0"])
    assert results[0] == b"v0"
    assert results[1] is None
    assert results[2] == b"v0"
    await db.aclose()


@pytest.mark.asyncio
async def test_put_many_with_list_keys(db_path):
    db = WaveDB(str(db_path))
    items = [(["users", str(i)], str(i)) for i in range(10)]
    await db.put_many(items)
    for i in range(10):
        assert db.get_sync(["users", str(i)]) == str(i).encode()
    await db.aclose()


# --- in_memory mode regression tests ---
# These verify that batch operations work when location=NULL (no WAL manager).
# Before the fix, database_write_batch_sync returned -1 when twal==NULL,
# causing sync batch to raise and async batch/put_many/delete_many to
# silently lose data (the async bridge didn't check the promise payload).


def test_batch_sync_in_memory(db_path):
    db = WaveDB(str(db_path), config=WaveDBConfig(in_memory=True))
    db.batch_sync([
        {"type": "put", "key": "a", "value": "1"},
        {"type": "put", "key": "b", "value": "2"},
    ])
    assert db.get_sync("a") == b"1"
    assert db.get_sync("b") == b"2"
    db.close()


@pytest.mark.asyncio
async def test_put_many_in_memory(db_path):
    db = WaveDB(str(db_path), config=WaveDBConfig(in_memory=True))
    items = [(f"k{i}", str(i)) for i in range(50)]
    await db.put_many(items)
    for i in range(50):
        assert await db.get(f"k{i}") == str(i).encode(), f"k{i} not persisted"
    await db.aclose()


@pytest.mark.asyncio
async def test_delete_many_in_memory(db_path):
    db = WaveDB(str(db_path), config=WaveDBConfig(in_memory=True))
    await db.put_many([(f"k{i}", str(i)) for i in range(20)])
    await db.delete_many([f"k{i}" for i in range(10)])
    for i in range(10):
        assert await db.get(f"k{i}") is None, f"k{i} should be deleted"
    for i in range(10, 20):
        assert await db.get(f"k{i}") == str(i).encode(), f"k{i} should remain"
    await db.aclose()


@pytest.mark.asyncio
async def test_get_many_in_memory(db_path):
    db = WaveDB(str(db_path), config=WaveDBConfig(in_memory=True))
    await db.put_many([(f"k{i}", str(i)) for i in range(10)])
    results = await db.get_many([f"k{i}" for i in range(10)])
    assert [r.decode() for r in results] == [str(i) for i in range(10)]
    await db.aclose()