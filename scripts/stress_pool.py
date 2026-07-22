"""Stress test for the memory_pool double-free / free-list corruption fix.

Does 200+ sequential batch_sync calls through the same WaveDB instance,
exercising the pool's alloc/free path heavily. Each call creates paths
and identifiers (pool-allocated), inserts them, and destroys them.

Checks:
  (a) no "possible double-free detected" warnings in stderr
  (b) no "batch_sync failed" errors on valid episodes
  (c) run twice for run-to-run stability
"""
import os
import sys
import tempfile
import warnings

import wavedb

EPISODES = 200
OPS_PER_EPISODE = 60  # mix of put/del


def run_once(label: str) -> tuple[int, int]:
    """Run EPISODES batch_sync calls. Returns (skips, double_free_warnings)."""
    d = tempfile.mkdtemp(prefix=f"wavedb_stress_{label}_")
    db = wavedb.WaveDB(os.path.join(d, "store"))
    skips = 0
    df_warnings = 0

    # Capture stderr to count double-free warnings
    import io
    import contextlib

    stderr_capture = io.StringIO()

    for ep in range(EPISODES):
        ops = []
        for i in range(OPS_PER_EPISODE):
            key = f"ep{ep}/seg{i}/key{i}"
            if i % 5 == 4:
                ops.append({"type": "del", "key": key})
            else:
                ops.append({"type": "put", "key": key, "value": f"val_{ep}_{i}"})
        try:
            with contextlib.redirect_stderr(stderr_capture):
                db.batch_sync(ops)
        except Exception as e:
            skips += 1
            print(f"  [{label}] episode {ep} FAILED: {e}", file=sys.stderr)

    # Check captured stderr for double-free warnings
    captured = stderr_capture.getvalue()
    df_warnings = captured.count("double-free")

    db.close()

    # Clean up
    import shutil
    shutil.rmtree(d, ignore_errors=True)

    return skips, df_warnings


if __name__ == "__main__":
    for run in range(2):
        label = f"run{run+1}"
        print(f"\n=== {label} ===", flush=True)
        skips, df = run_once(label)
        print(f"  Episodes: {EPISODES}", flush=True)
        print(f"  Skips (batch_sync failed): {skips}", flush=True)
        print(f"  Double-free warnings: {df}", flush=True)
        if skips > 0 or df > 0:
            print(f"  RESULT: FAIL", flush=True)
            sys.exit(1)
        else:
            print(f"  RESULT: PASS", flush=True)

    print("\nAll runs passed — no double-free warnings, no batch_sync failures.",
          flush=True)