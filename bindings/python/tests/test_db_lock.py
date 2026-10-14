"""Cross-process open lock (db_file_lock) tests.

Bug this guards: a second database open of the same directory used to be
allowed; at the second handle's close, persist+compaction rewound the
directory's persistence state to the second handle's (recovered) subset --
observed as silent loss of every record written before the second handle
opened. Now: the second open fails fast with DATABASE_ERR_LOCKED (-103),
and after the first handle closes, opening works and the data is intact.
"""

from __future__ import annotations

import subprocess
import sys
import textwrap

import pytest

from wavedb import WaveDB, WaveDBError


WRITER = textwrap.dedent(
    """
    import sys, wavedb
    wavedb.set_quiet(True)
    db = wavedb.WaveDB(sys.argv[1])
    db.put_sync("lock-test/a", b"one")
    db.put_sync("lock-test/b", b"two")
    import time
    time.sleep(6)
    db.put_sync("lock-test/c", b"three")
    db.close()
    """
)


def test_second_open_fails_and_data_survives(tmp_path):
    if sys.platform != "win32":
        pytest.skip("cross-process lock semantics tested on the primary platform")
    path = str(tmp_path / "lockdb")
    writer = subprocess.Popen([sys.executable, "-c", WRITER, path],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    import os
    import time

    # wait until the writer actually holds the db open (lock file exists)
    for _ in range(100):
        if os.path.exists(os.path.join(path, "db.wdbl")):
            break
        time.sleep(0.1)
    # concurrent open must fail
    with pytest.raises(WaveDBError) as err:
        db2 = WaveDB(path)
    assert "lock" in str(err.value).lower(), str(err.value)
    # the writer finishes (6s sleep) and closes; data intact
    writer.wait(timeout=60)
    db3 = WaveDB(path)
    try:
        assert db3.get_sync("lock-test/a") == b"one"
        assert db3.get_sync("lock-test/b") == b"two"
        assert db3.get_sync("lock-test/c") == b"three"
    finally:
        db3.close()


def test_sequential_reopen_still_works(tmp_path):
    path = str(tmp_path / "seqdb")
    db = WaveDB(path)
    db.put_sync("plain/k", b"v")
    db.close()
    db2 = WaveDB(path)
    try:
        assert db2.get_sync("plain/k") == b"v"
    finally:
        db2.close()