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
    assert c.worker_threads == 0
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