import pytest
from wavedb import WaveDB


def test_scan_sync(db_path):
    db = WaveDB(str(db_path))
    for i in range(5):
        db.put_sync(f"users/u{i}", str(i))
    out = list(db.create_read_stream(start="users/", end="users/~"))
    assert len(out) == 5
    assert out[0] == ("users/u0", b"0")
    db.close()


def test_scan_sync_with_limit(db_path):
    db = WaveDB(str(db_path))
    for i in range(10):
        db.put_sync(f"k{i}", str(i))
    out = list(db.create_read_stream(start="k", end="k~", limit=3))
    assert len(out) == 3
    db.close()


@pytest.mark.asyncio
async def test_scan_async(db_path):
    db = WaveDB(str(db_path))
    for i in range(5):
        await db.put(f"users/u{i}", str(i))
    out = []
    async for k, v in db.create_read_stream_async(start="users/", end="users/~"):
        out.append((k, v))
    assert len(out) == 5
    await db.aclose()
