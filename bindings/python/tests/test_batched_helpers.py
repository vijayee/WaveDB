"""Tests for batched async helpers (put_many, delete_many, get_many)."""
import pytest
from wavedb import WaveDB


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