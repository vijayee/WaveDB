import pytest
from wavedb import WaveDB


def test_put_object_sync_flattens(db_path):
    db = WaveDB(str(db_path))
    db.put_object_sync("users/alice", {
        "name": "Alice",
        "age": "30",
        "address": {"city": "SF", "zip": "94101"},
    })
    assert db.get_sync("users/alice/name") == b"Alice"
    assert db.get_sync("users/alice/age") == b"30"
    assert db.get_sync("users/alice/address/city") == b"SF"
    assert db.get_sync("users/alice/address/zip") == b"94101"
    db.close()


def test_get_object_sync_reconstructs(db_path):
    db = WaveDB(str(db_path))
    db.put_object_sync("users/alice", {
        "name": "Alice",
        "address": {"city": "SF"},
        "tags": ["a", "b"],
    })
    obj = db.get_object_sync("users/alice")
    assert obj["name"] == b"Alice"
    assert obj["address"]["city"] == b"SF"
    assert obj["tags"][0] == b"a"
    assert obj["tags"][1] == b"b"
    db.close()


@pytest.mark.asyncio
async def test_put_object_async(db_path):
    db = WaveDB(str(db_path))
    await db.put_object("u/bob", {"name": "Bob"})
    assert await db.get("u/bob/name") == b"Bob"
    obj = await db.get_object("u/bob")
    assert obj["name"] == b"Bob"
    await db.aclose()
