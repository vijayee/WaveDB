import pytest
from wavedb import WaveDB


def test_overwrite_sync(db_path):
    db = WaveDB(str(db_path))
    db.put_sync("k", "v1")
    db.put_sync("k", "v2")
    assert db.get_sync("k") == b"v2"
    db.close()


@pytest.mark.asyncio
async def test_overwrite_async(db_path):
    db = WaveDB(str(db_path))
    await db.put("k", "v1")
    await db.put("k", "v2")
    assert await db.get("k") == b"v2"
    await db.aclose()


def test_delete_tombstone_sync(db_path):
    db = WaveDB(str(db_path))
    db.put_sync("k", "v")
    db.del_sync("k")
    assert db.get_sync("k") is None
    # Re-put after delete
    db.put_sync("k", "v2")
    assert db.get_sync("k") == b"v2"
    db.close()


def test_close_after_overwrites_no_crash(db_path):
    """Regression: Node had a CBOR serialization crash on close after overwrites."""
    db = WaveDB(str(db_path))
    for i in range(20):
        db.put_sync("k", f"v{i}")
    db.close()  # must not crash