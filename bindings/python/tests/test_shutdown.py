"""Regression test for the interpreter-shutdown access violation.

Background: a reference cycle between ``WaveDB._open_subtrees`` and each
child handle's ``_db`` back-reference stranded both objects during
interpreter shutdown -- neither ``__del__`` ran, so the C
``database_destroy`` was never called and the background worker threads
(hierarchical timing wheel + work pool) were never stopped.  Those
threads crashed with an access violation (SIGSEGV / exit code 139) when
the interpreter tore down their memory out from under them.

The fix is an ``atexit`` hook (``_shutdown_close_all`` in database.py)
that closes every still-live WaveDB -- subtrees first, then the db --
early in shutdown, before the dangerous late phase.  These tests spawn
subprocesses that exit WITHOUT calling ``close()`` and assert a clean
exit code 0 (the segfault surfaces as 139 on POSIX, 0xC0000005 via the
``Windows fatal exception: access violation`` path on Windows -- either
way a non-zero / signalled exit).
"""
import os
import subprocess
import sys
import textwrap

import pytest

# A non-zero / signalled exit. On POSIX a SIGSEGV is 128 + 11 = 139. On
# Windows the process exits with an access-violation code. We treat any
# non-zero exit as a crash (the fix yields a clean 0).
def _run_subprocess(script: str, env: dict | None = None) -> int:
    """Run a snippet in a fresh interpreter and return its exit code.

    A fresh process is required: the access violation only surfaces when
    the interpreter that owns the WaveDB worker threads exits, and pytest
    itself must not be that interpreter.
    """
    code = textwrap.dedent(script)
    full_env = dict(os.environ)
    if env:
        full_env.update(env)
    # -X faulthandler so a crash is reported on stderr (aids debugging);
    # the assert below only checks the exit code.
    result = subprocess.run(
        [sys.executable, "-X", "faulthandler", "-c", code],
        capture_output=True,
        text=True,
        env=full_env,
        timeout=60,
    )
    return result.returncode


def test_exit_without_close_is_clean(tmp_path):
    """Opening WaveDB + VectorLayer and exiting without close() must not crash.

    This is the core regression: before the atexit fix, the db<->child
    reference cycle prevented ``__del__`` from running, the C worker
    threads were never stopped, and the process crashed on exit.
    """
    db_path = tmp_path / "seg.db"
    script = f"""
    import struct
    from wavedb import WaveDB, VectorLayer, Format, Runtime, IndexType, Distance
    db = WaveDB({str(db_path)!r})
    vl = VectorLayer.open("episodes", db,
        Format(IndexType.FLAT, 4, Distance.COSINE),
        Runtime(top_k=3, sync_only=1))
    vl.insert_sync("ep1", struct.pack("4f", 0.1, 0.2, 0.3, 0.4))
    vl.search_sync(struct.pack("4f", 0.1, 0.2, 0.3, 0.4), 2)
    # deliberately do NOT close -- atexit must clean up
    """
    rc = _run_subprocess(script)
    assert rc == 0, (
        f"expected clean exit 0, got {rc} (segfault/AV during shutdown). "
        "The atexit shutdown hook is not stopping the C worker threads."
    )


def test_exit_without_close_multiple_dbs(tmp_path):
    """Multiple live WaveDB instances (some with children, some without) plus
    an already-closed instance: atexit must close each in the right order
    (subtrees first) and skip closed ones without double-close."""
    a = tmp_path / "a.db"
    b = tmp_path / "b.db"
    c = tmp_path / "c.db"
    script = f"""
    import struct
    from wavedb import WaveDB, VectorLayer, Format, Runtime, IndexType, Distance
    db1 = WaveDB({str(a)!r}); db1.close()           # already closed -> skip
    db2 = WaveDB({str(b)!r})
    vl2 = VectorLayer.open("ep", db2,
        Format(IndexType.FLAT, 4, Distance.COSINE),
        Runtime(top_k=3, sync_only=1))
    vl2.insert_sync("e1", struct.pack("4f", 1, 0, 0, 0))
    db3 = WaveDB({str(c)!r})                         # no children -> direct close
    # deliberately do NOT close db2/vl2/db3
    """
    rc = _run_subprocess(script)
    assert rc == 0, (
        f"expected clean exit 0, got {rc} (multi-db shutdown crash). "
        "The atexit hook mishandles multiple/already-closed instances."
    )