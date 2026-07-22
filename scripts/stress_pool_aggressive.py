"""Aggressive stress test mimicking the HippocampalStore pattern.

The memory_pool double-free bug manifests with ~100+ sequential batch_sync
encodes through the HippocampalStore path, especially with many-segment paths
(memory note: "Section-count threshold ~12-ok/49+-fail"). This test creates
paths with many segments (mimicking documents with 49+ sections) and does
200+ sequential batch_sync calls.

Run twice for run-to-run stability (the original was flaky 0-vs-2 skips).
"""
import os
import sys
import tempfile
import shutil

import wavedb

EPISODES = 250
# Mimics a document with 49+ sections — each section is a path segment.
SECTIONS_PER_EPISODE = 50
SUBKEYS_PER_SECTION = 3  # content + 2 graph edges per section


def build_ops(ep_idx: int) -> list[dict]:
    """Build ops mimicking HippocampalStore.encode_episode for a 50-section doc."""
    ops = []
    for s in range(SECTIONS_PER_EPISODE):
        # Content put: multi-segment path like memory/doc_ep/section/sN/body
        path = f"memory/doc_ep{ep_idx}/section/s{s}/body"
        ops.append({"type": "put", "key": path, "value": f"content_{ep_idx}_{s}"})
        # Graph edge puts (mimics expand_triple)
        for k in range(SUBKEYS_PER_SECTION):
            edge_path = f"memory/doc_ep{ep_idx}/section/s{s}/edge{k}/target"
            ops.append({"type": "put", "key": edge_path, "value": f"edge_{ep_idx}_{s}_{k}"})
    return ops


def run_once(label: str) -> tuple[int, int, int]:
    """Run EPISODES batch_sync calls. Returns (skips, df_warnings, total_ops)."""
    d = tempfile.mkdtemp(prefix=f"wavedb_stress_{label}_")
    db = wavedb.WaveDB(os.path.join(d, "store"))
    skips = 0
    df_warnings = 0
    total_ops = 0

    import io
    import contextlib

    stderr_capture = io.StringIO()

    for ep in range(EPISODES):
        ops = build_ops(ep)
        total_ops += len(ops)
        try:
            with contextlib.redirect_stderr(stderr_capture):
                db.batch_sync(ops)
        except Exception as e:
            skips += 1
            print(f"  [{label}] episode {ep} FAILED: {e}", file=sys.stderr)

    captured = stderr_capture.getvalue()
    df_warnings = captured.count("double-free")

    db.close()
    shutil.rmtree(d, ignore_errors=True)

    return skips, df_warnings, total_ops


if __name__ == "__main__":
    all_pass = True
    for run in range(2):
        label = f"run{run+1}"
        print(f"\n=== {label} ===", flush=True)
        skips, df, ops = run_once(label)
        print(f"  Episodes: {EPISODES}", flush=True)
        print(f"  Total ops: {ops}", flush=True)
        print(f"  Skips (batch_sync failed): {skips}", flush=True)
        print(f"  Double-free warnings: {df}", flush=True)
        if skips > 0 or df > 0:
            print(f"  RESULT: FAIL", flush=True)
            all_pass = False
        else:
            print(f"  RESULT: PASS", flush=True)

    if all_pass:
        print("\nAll runs passed — no double-free warnings, no batch_sync failures.",
              flush=True)
        sys.exit(0)
    else:
        print("\nFAILURES detected — see above.", flush=True)
        sys.exit(1)