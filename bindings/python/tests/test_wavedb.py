import pytest
from wavedb import WaveDB, WaveDBError, InvalidPathError
from wavedb.config import WaveDBConfig


def test_put_get_sync_string_key(db_path):
    db = WaveDB(str(db_path))
    db.put_sync("users/alice/name", "Alice")
    assert db.get_sync("users/alice/name") == b"Alice"
    db.close()


def test_get_sync_missing_returns_none(db_path):
    db = WaveDB(str(db_path))
    assert db.get_sync("missing/key") is None
    db.close()


def test_put_sync_list_key(db_path):
    db = WaveDB(str(db_path))
    db.put_sync(["users", "bob", "age"], b"30")
    assert db.get_sync(["users", "bob", "age"]) == b"30"
    db.close()


def test_put_sync_bytes_value(db_path):
    db = WaveDB(str(db_path))
    db.put_sync("blob", b"\x00\x01\x02\x03")
    assert db.get_sync("blob") == b"\x00\x01\x02\x03"
    db.close()


def test_del_sync(db_path):
    db = WaveDB(str(db_path))
    db.put_sync("k", "v")
    db.del_sync("k")
    assert db.get_sync("k") is None
    db.close()


def test_custom_delimiter(db_path):
    db = WaveDB(str(db_path), delimiter=":")
    db.put_sync("users:alice:name", "Alice")
    assert db.get_sync("users:alice:name") == b"Alice"
    db.close()


def test_invalid_empty_key_raises(db_path):
    db = WaveDB(str(db_path))
    with pytest.raises(InvalidPathError):
        db.put_sync("", "v")
    db.close()


def test_close_then_op_raises(db_path):
    db = WaveDB(str(db_path))
    db.close()
    with pytest.raises(WaveDBError):
        db.put_sync("k", "v")


def test_context_manager(db_path):
    with WaveDB(str(db_path)) as db:
        db.put_sync("k", "v")
        assert db.get_sync("k") == b"v"


def test_explicit_default_config(db_path):
    # WaveDBConfig() with all defaults should produce a usable database
    db = WaveDB(str(db_path), config=WaveDBConfig())
    db.put_sync("k", "v")
    assert db.get_sync("k") == b"v"
    db.close()


def test_multibyte_delimiter_raises(db_path):
    with pytest.raises(InvalidPathError):
        WaveDB(str(db_path), delimiter="é")
