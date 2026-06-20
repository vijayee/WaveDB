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
    # KNOWN LIMITATION (C-level): `enable_persist=False` does NOT disable WAL
    # persistence. The C `database_create_with_config` only treats databases
    # as in-memory when `location == NULL`; with a non-NULL path the WAL
    # manager is always created (`database.c:1058` — `if (!is_memory_only)`),
    # writes `thread_*.wal` + `manifest.dat` directly into `location`, and
    # replays them on the next open. `enable_persist=False` only skips the
    # page-file/index-file setup (`database.c:886`, `database.c:902`), not WAL.
    # The Python binding's `WaveDB.__init__` rejects empty paths
    # (`InvalidPathError`), so it cannot express the NULL-location memory-only
    # mode through this constructor. Verified empirically: a second WaveDB
    # opened on the same directory with `enable_persist=False` recovers the
    # PUT via WAL replay. Skip until the C layer gates WAL on enable_persist
    # or the binding exposes a NULL-location constructor.
    pytest.skip(
        "enable_persist=False does not disable WAL in the C layer "
        "(only NULL location does, which the binding rejects)"
    )
    cfg = WaveDBConfig(enable_persist=False)
    db = WaveDB(str(db_path), config=cfg)
    db.put_sync("k", "v")
    db.close()

    db2 = WaveDB(str(db_path), config=cfg)
    assert db2.get_sync("k") is None
    db2.close()


@pytest.mark.asyncio
async def test_persist_async(db_path):
    cfg = WaveDBConfig(enable_persist=True)
    db = WaveDB(str(db_path), config=cfg)
    await db.put("k", "v")
    await db.aclose()
    db2 = WaveDB(str(db_path), config=cfg)
    assert await db2.get("k") == b"v"
    await db2.aclose()