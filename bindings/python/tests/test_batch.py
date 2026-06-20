import pytest
from wavedb import WaveDB, InvalidPathError


def test_batch_sync_mixed(db_path):
    db = WaveDB(str(db_path))
    db.put_sync("keep", "v1")
    db.batch_sync([
        {"type": "put", "key": "a", "value": "1"},
        {"type": "put", "key": "b", "value": b"2"},
        {"type": "del", "key": "keep"},
    ])
    assert db.get_sync("a") == b"1"
    assert db.get_sync("b") == b"2"
    assert db.get_sync("keep") is None
    db.close()


def test_batch_sync_empty(db_path):
    db = WaveDB(str(db_path))
    db.batch_sync([])
    db.close()


def test_batch_sync_rejects_unknown_type(db_path):
    db = WaveDB(str(db_path))
    with pytest.raises(ValueError):
        db.batch_sync([{"type": "upsert", "key": "a", "value": "1"}])
    db.close()


def test_batch_sync_list_keys(db_path):
    db = WaveDB(str(db_path))
    db.batch_sync([
        {"type": "put", "key": ["x", "y"], "value": "v"},
    ])
    assert db.get_sync(["x", "y"]) == b"v"
    db.close()


def test_batch_sync_missing_key_raises(db_path):
    db = WaveDB(str(db_path))
    with pytest.raises(ValueError, match="missing required 'key'"):
        db.batch_sync([{"type": "put", "value": "1"}])
    db.close()


def test_batch_sync_missing_value_raises(db_path):
    db = WaveDB(str(db_path))
    with pytest.raises(ValueError, match="missing required 'value'"):
        db.batch_sync([{"type": "put", "key": "a"}])
    db.close()
