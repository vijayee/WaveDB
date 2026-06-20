import pytest
from wavedb.config import WaveDBConfig, WaveDBEncryption


def test_default_config():
    c = WaveDBConfig()
    assert c.chunk_size == 4
    assert c.btree_node_size == 4096
    assert c.enable_persist is True
    assert c.lru_memory_mb == 50
    assert c.lru_shards == 0
    assert c.wal_sync_mode == "debounced"
    assert c.wal_debounce_ms == 250
    assert c.worker_threads == 4
    assert c.sync_only is False


def test_encryption_defaults():
    e = WaveDBEncryption(type="aes-256-gcm")
    assert e.type == "aes-256-gcm"
    assert e.symmetric_key is None
    assert e.asymmetric_private_key is None
    assert e.asymmetric_public_key is None


def test_wal_sync_mode_values():
    c = WaveDBConfig(wal_sync_mode="immediate")
    assert c.wal_sync_mode == "immediate"


def test_invalid_wal_sync_mode_raises():
    with pytest.raises(ValueError):
        WaveDBConfig(wal_sync_mode="bogus")


def test_chunk_size_zero_raises():
    with pytest.raises(ValueError):
        WaveDBConfig(chunk_size=0)


def test_chunk_size_too_large_raises():
    with pytest.raises(ValueError):
        WaveDBConfig(chunk_size=256)


def test_chunk_size_boundary_255_succeeds():
    c = WaveDBConfig(chunk_size=255)
    assert c.chunk_size == 255


def test_wal_sync_mode_none_succeeds():
    c = WaveDBConfig(wal_sync_mode="none")
    assert c.wal_sync_mode == "none"


def test_negative_btree_node_size_raises():
    with pytest.raises(ValueError):
        WaveDBConfig(btree_node_size=-1)


def test_negative_lru_memory_mb_raises():
    with pytest.raises(ValueError):
        WaveDBConfig(lru_memory_mb=-1)


def test_negative_worker_threads_raises():
    with pytest.raises(ValueError):
        WaveDBConfig(worker_threads=-1)


def test_worker_threads_above_255_raises():
    with pytest.raises(ValueError):
        WaveDBConfig(worker_threads=256)


def test_encryption_empty_type_raises():
    with pytest.raises(ValueError):
        WaveDBEncryption(type="")


def test_encryption_non_string_type_raises():
    with pytest.raises(ValueError):
        WaveDBEncryption(type=123)  # type: ignore[arg-type]
