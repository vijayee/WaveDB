import pytest

from wavedb import WaveDB, WaveDBEncryption, EncryptionError


def test_encrypted_db_round_trip(db_path):
    enc = WaveDBEncryption(
        type="aes-256-gcm",
        symmetric_key=b"0123456789abcdef0123456789abcdef",
    )
    db = WaveDB(str(db_path), encryption=enc)
    db.put_sync("secret", "data")
    assert db.get_sync("secret") == b"data"
    db.close()


def test_encrypted_db_reopen(db_path):
    enc = WaveDBEncryption(
        type="aes-256-gcm",
        symmetric_key=b"0123456789abcdef0123456789abcdef",
    )
    db1 = WaveDB(str(db_path), encryption=enc)
    db1.put_sync("k", "v")
    db1.close()
    db2 = WaveDB(str(db_path), encryption=enc)
    assert db2.get_sync("k") == b"v"
    db2.close()


def test_invalid_key_raises(db_path):
    enc = WaveDBEncryption(type="aes-256-gcm", symmetric_key=b"short")
    with pytest.raises(EncryptionError):
        WaveDB(str(db_path), encryption=enc)


def test_wrong_key_on_reopen_raises(db_path):
    enc = WaveDBEncryption(
        type="aes-256-gcm",
        symmetric_key=b"0123456789abcdef0123456789abcdef",
    )
    db1 = WaveDB(str(db_path), encryption=enc)
    db1.put_sync("k", "v")
    db1.close()
    wrong = WaveDBEncryption(
        type="aes-256-gcm",
        symmetric_key=b"fedcba9876543210fedcba9876543210",
    )
    with pytest.raises(EncryptionError):
        WaveDB(str(db_path), encryption=wrong)
