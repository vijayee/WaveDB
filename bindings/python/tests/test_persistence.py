import pytest

from wavedb import WaveDB, WaveDBConfig


def test_persist_close_reopen(db_path):
    cfg = WaveDBConfig(enable_persist=True)
    db = WaveDB(str(db_path), config=cfg)
    db.put_sync("k1", "v1")
    db.put_sync("k2", "v2")
    db.close()

    db2 = WaveDB(str(db_path), config=cfg)
    # WAL recovery should restore the data
    assert db2.get_sync("k1") == b"v1"
    assert db2.get_sync("k2") == b"v2"
    db2.close()


def test_in_memory_not_persisted(db_path):
    # in_memory=True passes location=NULL to C, which sets is_memory_only=true
    # — no WAL, no page file, no config save. Data is truly ephemeral.
    cfg = WaveDBConfig(in_memory=True)
    db = WaveDB(str(db_path), config=cfg)
    db.put_sync("k", "v")
    db.close()

    # Reopen with persistence — no data should survive (no WAL was written)
    cfg2 = WaveDBConfig(enable_persist=True)
    db2 = WaveDB(str(db_path), config=cfg2)
    assert db2.get_sync("k") is None
    db2.close()


def test_in_memory_round_trip(db_path):
    # In-memory database: data available during the session, lost on close
    cfg = WaveDBConfig(in_memory=True)
    db = WaveDB(str(db_path), config=cfg)
    db.put_sync("k1", "v1")
    db.put_sync("k2", "v2")
    assert db.get_sync("k1") == b"v1"
    assert db.get_sync("k2") == b"v2"
    db.close()


@pytest.mark.asyncio
async def test_persist_async(db_path):
    cfg = WaveDBConfig(enable_persist=True)
    db = WaveDB(str(db_path), config=cfg)
    await db.put("k", "v")
    await db.aclose()
    db2 = WaveDB(str(db_path), config=cfg)
    assert await db2.get("k") == b"v"
    await db2.aclose()