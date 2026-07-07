"""Tests for vacuum configuration and WaveDB.vacuum()/flush().

Vacuum reclaims stale bytes from the page file after overwrites/deletes.
The data file (`data.wdbp`) lives in the directory passed to WaveDB() —
`database_create_with_config` appends `/data.wdbp` internally.

Without `flush()` first, vacuum has nothing to compact because dirty bnodes
haven't been written to the page file yet, and close() may hang if there
are pending dirty bnodes.
"""
import os
import shutil
import tempfile

import pytest

from wavedb import WaveDB, WaveDBConfig
from wavedb.config import VacuumConfig, VacuumMode


@pytest.fixture
def vac_dir():
    tmpdir = tempfile.mkdtemp(prefix="wavedb_py_vacuum_")
    yield tmpdir
    shutil.rmtree(tmpdir, ignore_errors=True)


def _data_file_size(vac_dir: str) -> int:
    data_file = os.path.join(vac_dir, "data.wdbp")
    return os.path.getsize(data_file)


def test_vacuum_shrinks_after_overwrite(vac_dir):
    cfg_kwargs = dict(
        vacuum_config=VacuumConfig(
            mode=VacuumMode.manual_only,
            min_file_size_bytes=0,
            min_stale_bytes=0,
        ),
    )
    db = WaveDB(vac_dir, config=WaveDBConfig(sync_only=True), **cfg_kwargs)
    try:
        for i in range(200):
            db.put_sync(f"k/{i}", b"v0")
        db.flush()
        for rep in range(5):
            for i in range(200):
                db.put_sync(f"k/{i}", f"v{rep}".encode())
            db.flush()

        before = _data_file_size(vac_dir)
        rc = db.vacuum()
        assert rc == 0
        after = _data_file_size(vac_dir)
        assert after < before, f"file did not shrink: {before} -> {after}"

        for i in range(200):
            assert db.get_sync(f"k/{i}") == b"v4"
    finally:
        db.close()


def test_vacuum_preserves_keys_after_delete(vac_dir):
    """After deletes + vacuum, surviving keys must still be readable."""
    db = WaveDB(
        vac_dir,
        config=WaveDBConfig(sync_only=True),
        vacuum_config=VacuumConfig(
            mode=VacuumMode.manual_only,
            min_file_size_bytes=0,
            min_stale_bytes=0,
        ),
    )
    try:
        for i in range(100):
            db.put_sync(f"k/{i}", b"v")
        db.flush()
        for i in range(0, 100, 2):  # delete every other key
            db.del_sync(f"k/{i}")
        db.flush()

        rc = db.vacuum()
        assert rc == 0

        for i in range(100):
            if i % 2 == 0:
                assert db.get_sync(f"k/{i}") is None
            else:
                assert db.get_sync(f"k/{i}") == b"v"
    finally:
        db.close()


def test_vacuum_returns_zero_on_fresh_db(vac_dir):
    """A freshly-created database with no stale bytes should still vacuum cleanly."""
    db = WaveDB(
        vac_dir,
        config=WaveDBConfig(sync_only=True),
        vacuum_config=VacuumConfig(
            mode=VacuumMode.manual_only,
            min_file_size_bytes=0,
            min_stale_bytes=0,
        ),
    )
    try:
        db.put_sync("k", "v")
        db.flush()
        # No stale bytes yet — vacuum is a no-op but returns 0.
        assert db.vacuum() == 0
        assert db.get_sync("k") == b"v"
    finally:
        db.close()


def test_default_vacuum_config_strict_mode():
    cfg = VacuumConfig()
    assert cfg.mode == VacuumMode.strict
    assert cfg.stale_threshold > 0
    assert cfg.min_file_size_bytes > 0


def test_vacuum_status_fields(vac_dir):
    """vacuum_status() exposes file_size, stale_bytes, stale_ratio,
    vacuum_in_progress, open_cursor_count, and would_trigger — the same
    signals the auto-triggers evaluate."""
    db = WaveDB(
        vac_dir,
        config=WaveDBConfig(sync_only=True),
        vacuum_config=VacuumConfig(
            mode=VacuumMode.manual_only,
            min_file_size_bytes=0,
            min_stale_bytes=0,
        ),
    )
    try:
        # Fresh DB: no stale bytes, no cursors, no vacuum in progress.
        s = db.vacuum_status()
        assert isinstance(s, dict)
        assert isinstance(s['stale_ratio'], float)
        assert isinstance(s['would_trigger'], bool)
        assert s['vacuum_in_progress'] is False
        assert s['open_cursor_count'] == 0
        assert s['stale_bytes'] == 0
        assert s['stale_ratio'] == 0.0

        # Write + overwrite to grow stale.
        for i in range(100):
            db.put_sync(f"k/{i}", b"v0")
        db.flush()
        for rep in range(1, 6):
            for i in range(100):
                db.put_sync(f"k/{i}", f"v{rep}".encode())
            db.flush()

        s2 = db.vacuum_status()
        assert s2['stale_bytes'] > 0, f"stale_bytes should be > 0: {s2}"
        assert s2['stale_ratio'] > 0.0
        assert s2['file_size'] > 0
        assert s2['vacuum_in_progress'] is False
        assert s2['open_cursor_count'] == 0
    finally:
        db.close()