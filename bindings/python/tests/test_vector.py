"""Tests for the VectorLayer Python cffi binding.

Mirrors the C++ gtest coverage in tests/test_vector.cpp via the wrapper
API in bindings/python/src/wavedb/vector_layer.py. Covers:
  - lifecycle (open_separate / open shared / close / count)
  - Format/Runtime split + reconfigure (Runtime only)
  - error mapping (invalid dim)
  - FLAT insert / search / delete / metadata
  - IVF insert / train / rebuild / search
  - SLSH insert / train / rebuild / search (bidirectional scan)
  - recall sanity (top-1 contains the query vector itself)

Async API (promise_t*) is deferred — bridging the C promise callback to
a Python future needs a trampoline and is out of scope for the spike.
The sync API is wired + tested here.
"""
import os
import random

import pytest

from wavedb import (
    Distance,
    Format,
    IndexType,
    Runtime,
    VectorLayer,
    WaveDB,
    WaveDBError,
    set_log_callback,
    set_progress_handler,
    set_quiet,
)


# ---- helpers ----


def _flat_format(dim: int = 4, distance: int = Distance.L2) -> Format:
    return Format(index_type=IndexType.FLAT, dim=dim, distance=distance)


def _ivf_format(dim: int = 4, n_clusters: int = 3) -> Format:
    return Format(
        index_type=IndexType.IVF,
        dim=dim,
        distance=Distance.L2,
        ivf_n_clusters=n_clusters,
    )


def _slsh_format(dim: int = 4) -> Format:
    return Format(
        index_type=IndexType.SLSH,
        dim=dim,
        distance=Distance.L2,
        slsh_lsh_tables=2,
        slsh_hash_bits=8,
        slsh_bucket_width=1.0,
    )


# ---- lifecycle ----


def test_open_separate_and_count(tmp_path):
    fmt = _flat_format()
    rt = Runtime(sync_only=1)
    vl = VectorLayer.open_separate(str(tmp_path / "vltest"), "test", fmt, rt)
    assert vl.count() == 0
    vl.close()


def test_open_shared_on_existing_db(tmp_path):
    db = WaveDB(str(tmp_path / "db"))
    fmt = _flat_format(distance=Distance.COSINE)
    rt = Runtime(sync_only=1)
    vl = VectorLayer.open("test", db, fmt, rt)
    assert vl.count() == 0
    vl.close()
    db.close()


def test_context_manager_closes(tmp_path):
    fmt = _flat_format()
    rt = Runtime(sync_only=1)
    with VectorLayer.open_separate(str(tmp_path / "vltest"), "test", fmt, rt) as vl:
        assert vl.count() == 0
    # second close is a no-op
    vl.close()


# ---- reconfigure (Runtime only) ----


def test_reconfigure_runtime(tmp_path):
    fmt = _ivf_format()
    rt = Runtime(sync_only=1, ivf_nprobe=4)
    vl = VectorLayer.open_separate(str(tmp_path / "vltest"), "test", fmt, rt)
    rt2 = Runtime(sync_only=1, ivf_nprobe=8, ivf_flat_until=500)
    assert vl.reconfigure(rt2) == 0
    vl.close()


# ---- error mapping ----


def test_invalid_dim_rejected(tmp_path):
    fmt = Format(index_type=IndexType.FLAT, dim=0, distance=Distance.L2)
    rt = Runtime(sync_only=1)
    with pytest.raises(Exception):
        VectorLayer.open_separate(str(tmp_path / "vltest"), "test", fmt, rt)


def test_empty_index_name_rejected(tmp_path):
    fmt = _flat_format()
    rt = Runtime(sync_only=1)
    with pytest.raises(Exception):
        VectorLayer.open_separate(str(tmp_path / "vltest"), "", fmt, rt)


# ---- FLAT insert / search / metadata ----


def test_flat_insert_count(tmp_path):
    fmt = _flat_format()
    rt = Runtime(sync_only=1)
    vl = VectorLayer.open_separate(str(tmp_path / "vltest"), "test", fmt, rt)
    v = [1.0, 2.0, 3.0, 4.0]
    for i in range(5):
        assert vl.insert_sync(f"v{i}", v) == 0
    assert vl.count() == 5
    vl.close()


def test_flat_search_returns_nearest(tmp_path):
    fmt = _flat_format()
    rt = Runtime(sync_only=1)
    vl = VectorLayer.open_separate(str(tmp_path / "vltest"), "test", fmt, rt)
    vl.insert_sync("a", [1.0, 2.0, 3.0, 4.0])
    vl.insert_sync("b", [4.0, 3.0, 2.0, 1.0])
    results = vl.search_sync([1.0, 2.0, 3.0, 4.0], 2)
    assert len(results) == 2
    assert results[0].id_str == "a"
    # distance is non-negative for L2; nearest has the smallest distance.
    assert results[0].distance <= results[1].distance
    vl.close()


def test_flat_metadata_roundtrip(tmp_path):
    fmt = _flat_format()
    rt = Runtime(sync_only=1)
    vl = VectorLayer.open_separate(str(tmp_path / "vltest"), "test", fmt, rt)
    vl.insert_sync("a", [1.0, 2.0, 3.0, 4.0], metadata=b"\xaa\xbb\xcc")
    results = vl.search_sync([1.0, 2.0, 3.0, 4.0], 1)
    assert len(results) == 1
    assert results[0].id_str == "a"
    assert results[0].metadata == b"\xaa\xbb\xcc"
    vl.close()


def test_flat_metadata_empty_when_none(tmp_path):
    fmt = _flat_format()
    rt = Runtime(sync_only=1)
    vl = VectorLayer.open_separate(str(tmp_path / "vltest"), "test", fmt, rt)
    vl.insert_sync("a", [1.0, 2.0, 3.0, 4.0])
    results = vl.search_sync([1.0, 2.0, 3.0, 4.0], 1)
    assert len(results) == 1
    assert results[0].metadata == b""
    vl.close()


def test_flat_delete(tmp_path):
    fmt = _flat_format()
    rt = Runtime(sync_only=1)
    vl = VectorLayer.open_separate(str(tmp_path / "vltest"), "test", fmt, rt)
    vl.insert_sync("a", [1.0, 2.0, 3.0, 4.0])
    vl.insert_sync("b", [4.0, 3.0, 2.0, 1.0])
    assert vl.count() == 2
    assert vl.delete_sync("a") == 0
    assert vl.count() == 1
    results = vl.search_sync([1.0, 2.0, 3.0, 4.0], 2)
    ids = {r.id_str for r in results}
    assert "a" not in ids
    assert "b" in ids
    vl.close()


def test_flat_recall_self_top1(tmp_path):
    """Insert N distinct vectors; searching for each should return itself as top-1."""
    fmt = _flat_format(dim=4)
    rt = Runtime(sync_only=1)
    vl = VectorLayer.open_separate(str(tmp_path / "vltest"), "test", fmt, rt)
    random.seed(42)
    items = []
    for i in range(10):
        v = [random.uniform(-1, 1) for _ in range(4)]
        vl.insert_sync(f"v{i}", v)
        items.append((f"v{i}", v))
    hits = 0
    for vid, v in items:
        res = vl.search_sync(v, 1)
        if res and res[0].id_str == vid:
            hits += 1
    # FLAT is exact — self must be top-1 for every query.
    assert hits == 10
    vl.close()


# ---- IVF ----


def test_ivf_insert_count(tmp_path):
    fmt = _ivf_format(n_clusters=3)
    rt = Runtime(sync_only=1, ivf_nprobe=2, ivf_flat_until=1000)
    vl = VectorLayer.open_separate(str(tmp_path / "vltest"), "test", fmt, rt)
    v = [1.0, 2.0, 3.0, 4.0]
    for i in range(5):
        assert vl.insert_sync(f"v{i}", v) == 0
    assert vl.count() == 5
    vl.close()


def test_ivf_search_after_train(tmp_path):
    fmt = _ivf_format(n_clusters=3)
    rt = Runtime(sync_only=1, ivf_nprobe=2, ivf_flat_until=1000)
    vl = VectorLayer.open_separate(str(tmp_path / "vltest"), "test", fmt, rt)
    random.seed(7)
    centers = [[10.0, 0.0, 0.0, 0.0], [0.0, 10.0, 0.0, 0.0], [0.0, 0.0, 10.0, 0.0]]
    for i in range(20):
        c = i % 3
        v = [centers[c][d] + random.uniform(0, 1) for d in range(4)]
        vl.insert_sync(f"v{i}", v)
    assert vl.train() == 0
    q = [10.0, 0.0, 0.0, 0.0]
    results = vl.search_sync(q, 5)
    assert len(results) > 0
    for r in results:
        # all top results should be near cluster 0
        assert r.distance < 5.0
    vl.close()


def test_ivf_train_rebuild(tmp_path):
    fmt = _ivf_format(n_clusters=3)
    rt = Runtime(sync_only=1, ivf_nprobe=3, ivf_flat_until=5)
    vl = VectorLayer.open_separate(str(tmp_path / "vltest"), "test", fmt, rt)
    random.seed(7)
    centers = [[10.0, 0.0, 0.0, 0.0], [0.0, 10.0, 0.0, 0.0], [0.0, 0.0, 10.0, 0.0]]
    for i in range(20):
        c = i % 3
        v = [centers[c][d] + random.uniform(0, 1) for d in range(4)]
        vl.insert_sync(f"v{i}", v)
    assert vl.train() == 0
    assert vl.rebuild() == 0
    q = [10.0, 0.0, 0.0, 0.0]
    results = vl.search_sync(q, 5)
    assert len(results) > 0
    for r in results:
        assert r.distance < 5.0
    vl.close()


# ---- SLSH ----


def test_slsh_insert_count(tmp_path):
    fmt = _slsh_format()
    rt = Runtime(sync_only=1, slsh_scan_radius=10)
    vl = VectorLayer.open_separate(str(tmp_path / "vltest"), "test", fmt, rt)
    v = [1.0, 2.0, 3.0, 4.0]
    for i in range(5):
        assert vl.insert_sync(f"v{i}", v) == 0
    assert vl.count() == 5
    vl.close()


def test_slsh_search_recall(tmp_path):
    """SLSH is approximate, but a self-query on a small corpus must return the
    query vector itself within top-k (sanity, not a strict recall gate)."""
    fmt = _slsh_format()
    rt = Runtime(sync_only=1, slsh_scan_radius=50)
    vl = VectorLayer.open_separate(str(tmp_path / "vltest"), "test", fmt, rt)
    random.seed(11)
    items = []
    for i in range(15):
        v = [random.uniform(-5, 5) for _ in range(4)]
        vl.insert_sync(f"v{i}", v)
        items.append((f"v{i}", v))
    assert vl.train() == 0
    # At least one self-query should land in top-5 (recall sanity).
    hits = 0
    for vid, v in items:
        res = vl.search_sync(v, 5)
        if any(r.id_str == vid for r in res):
            hits += 1
    assert hits > 0
    vl.close()


def test_slsh_train_rebuild(tmp_path):
    fmt = _slsh_format()
    rt = Runtime(sync_only=1, slsh_scan_radius=20)
    vl = VectorLayer.open_separate(str(tmp_path / "vltest"), "test", fmt, rt)
    random.seed(13)
    for i in range(10):
        v = [random.uniform(-3, 3) for _ in range(4)]
        vl.insert_sync(f"v{i}", v)
    assert vl.train() == 0
    assert vl.rebuild() == 0
    assert vl.count() == 10
    vl.close()


# ---- type checks ----


def test_insert_rejects_non_str_id(tmp_path):
    fmt = _flat_format()
    rt = Runtime(sync_only=1)
    vl = VectorLayer.open_separate(str(tmp_path / "vltest"), "test", fmt, rt)
    with pytest.raises(TypeError):
        vl.insert_sync(b"bytes-not-str", [1.0, 2.0, 3.0, 4.0])
    vl.close()


def test_insert_rejects_non_bytes_metadata(tmp_path):
    fmt = _flat_format()
    rt = Runtime(sync_only=1)
    vl = VectorLayer.open_separate(str(tmp_path / "vltest"), "test", fmt, rt)
    with pytest.raises(TypeError):
        vl.insert_sync("a", [1.0, 2.0, 3.0, 4.0], metadata="not-bytes")
    vl.close()


# ---- __format persistence + open-time validation ----


def test_create_writes_format(tmp_path):
    fmt = _flat_format()
    rt = Runtime(sync_only=1)
    vl = VectorLayer.open_separate(str(tmp_path / "vltest"), "test", fmt, rt)
    gf = vl.get_format()
    assert gf.index_type == IndexType.FLAT
    assert gf.dim == 4
    vl.close()


def test_reopen_validates_matching_type(tmp_path):
    path = str(tmp_path / "vltest")
    fmt = _flat_format()
    rt = Runtime(sync_only=1)
    vl = VectorLayer.open_separate(path, "test", fmt, rt)
    vl.insert_sync("v0", [1.0, 2.0, 3.0, 4.0])
    vl.close()
    # Reopen with the SAME type -> ok, data preserved.
    vl2 = VectorLayer.open_separate(path, "test", _flat_format(), rt)
    assert vl2.count() == 1
    assert vl2.get_format().index_type == IndexType.FLAT
    vl2.close()


def test_reopen_mismatched_type_raises(tmp_path):
    path = str(tmp_path / "vltest")
    rt = Runtime(sync_only=1)
    vl = VectorLayer.open_separate(path, "test", _flat_format(), rt)
    vl.insert_sync("v0", [1.0, 2.0, 3.0, 4.0])
    vl.close()
    # Reopen with a DIFFERENT type (same dim) -> loud error, NOT silent degrade.
    with pytest.raises(WaveDBError):
        VectorLayer.open_separate(path, "test", _ivf_format(n_clusters=2), rt)


def test_legacy_index_upgrades_on_reopen(tmp_path):
    """A pre-0.2.1 index has no __format leaf. First reopen with a type writes
    it (self-describing upgrade); a subsequent mismatched reopen then raises.
    """
    path = str(tmp_path / "vltest")
    rt = Runtime(sync_only=1)
    vl = VectorLayer.open_separate(path, "test", _flat_format(), rt)
    vl.insert_sync("v0", [1.0, 2.0, 3.0, 4.0])
    vl.close()
    # Strip the __format leaf -> simulate a 0.2.0 index (count + vector only).
    db = WaveDB(path)
    db.del_sync("vec/test/__format")
    db.close()
    # Reopen with FLAT -> absent leaf is written from the passed type; ok.
    vl2 = VectorLayer.open_separate(path, "test", _flat_format(), rt)
    assert vl2.get_format().index_type == IndexType.FLAT
    assert vl2.count() == 1
    vl2.close()
    # Now __format="flat" on disk -> reopening as IVF raises (loud mismatch).
    with pytest.raises(WaveDBError):
        VectorLayer.open_separate(path, "test", _ivf_format(n_clusters=2), rt)


# ---- migrate ----

# Three well-separated 4-d centers (mirrors test_ivf_search_after_train).
_CENTERS = [[10.0, 0.0, 0.0, 0.0], [0.0, 10.0, 0.0, 0.0], [0.0, 0.0, 10.0, 0.0]]


def _insert_clustered(vl, n_per_cluster=20, seed=7):
    random.seed(seed)
    stored = {}
    i = 0
    for c in range(3):
        for _ in range(n_per_cluster):
            v = [_CENTERS[c][d] + random.uniform(0, 1) for d in range(4)]
            vl.insert_sync(f"v{i}", v)
            stored[f"v{i}"] = v
            i += 1
    return stored


def _ivf_rt(nprobe=3):
    return Runtime(sync_only=1, ivf_nprobe=nprobe, ivf_flat_until=1000)


def test_migrate_flat_to_ivf_preserves_count_and_search(tmp_path):
    path = str(tmp_path / "vltest")
    vl = VectorLayer.open_separate(path, "test", _flat_format(), Runtime(sync_only=1))
    _insert_clustered(vl)
    n = vl.count()
    assert n == 60
    set_quiet(True)
    vl.migrate(_ivf_format(n_clusters=3))
    assert vl.count() == n
    assert vl.get_format().index_type == IndexType.IVF
    vl.reconfigure(_ivf_rt(nprobe=3))
    res = vl.search_sync([10.0, 0.0, 0.0, 0.0], 5)
    assert len(res) > 0
    for r in res:
        assert r.distance < 5.0  # all near cluster 0
    vl.close()


def test_migrate_flat_to_slsh_preserves_count(tmp_path):
    path = str(tmp_path / "vltest")
    vl = VectorLayer.open_separate(path, "test", _flat_format(), Runtime(sync_only=1))
    _insert_clustered(vl)
    n = vl.count()
    set_quiet(True)
    vl.migrate(_slsh_format())
    assert vl.count() == n
    assert vl.get_format().index_type == IndexType.SLSH
    vl.close()


def test_migrate_ivf_to_flat(tmp_path):
    path = str(tmp_path / "vltest")
    vl = VectorLayer.open_separate(path, "test", _ivf_format(n_clusters=3), _ivf_rt())
    _insert_clustered(vl)
    vl.train()
    n = vl.count()
    set_quiet(True)
    vl.migrate(_flat_format())
    assert vl.count() == n
    assert vl.get_format().index_type == IndexType.FLAT
    # FLAT is exact: a self-query's top-1 is itself.
    q = [10.5, 0.2, 0.1, 0.0]
    vl.insert_sync("selfq", q)
    res = vl.search_sync(q, 1)
    assert res[0].id_str == "selfq"
    vl.close()


def test_migrate_rejects_dim_mismatch(tmp_path):
    path = str(tmp_path / "vltest")
    vl = VectorLayer.open_separate(path, "test", _flat_format(dim=4), Runtime(sync_only=1))
    vl.insert_sync("v0", [1.0, 2.0, 3.0, 4.0])
    set_quiet(True)
    with pytest.raises(WaveDBError):
        vl.migrate(Format(index_type=IndexType.IVF, dim=8, ivf_n_clusters=2))
    # Layer is unchanged and still usable.
    assert vl.get_format().index_type == IndexType.FLAT
    assert vl.count() == 1
    res = vl.search_sync([1.0, 2.0, 3.0, 4.0], 1)
    assert res[0].id_str == "v0"
    vl.close()


def test_migrate_same_type_rebuilds_in_place(tmp_path):
    path = str(tmp_path / "vltest")
    vl = VectorLayer.open_separate(path, "test", _ivf_format(n_clusters=3), _ivf_rt())
    _insert_clustered(vl)
    vl.train()
    n = vl.count()
    set_quiet(True)
    # IVF -> IVF (same params): no short-circuit; aux rebuilt in place.
    vl.migrate(_ivf_format(n_clusters=3))
    assert vl.count() == n
    assert vl.get_format().index_type == IndexType.IVF
    res = vl.search_sync([10.0, 0.0, 0.0, 0.0], 5)
    assert len(res) > 0
    vl.close()


def test_migrate_preserves_metadata(tmp_path):
    path = str(tmp_path / "vltest")
    vl = VectorLayer.open_separate(path, "test", _flat_format(), Runtime(sync_only=1))
    random.seed(99)
    metas = {}
    vecs = {}
    for i in range(15):
        c = i % 3
        v = [_CENTERS[c][d] + random.uniform(0, 1) for d in range(4)]
        m = bytes([i, i ^ 0xFF])
        vl.insert_sync(f"v{i}", v, metadata=m)
        metas[f"v{i}"] = m
        vecs[f"v{i}"] = v
    set_quiet(True)
    vl.migrate(_ivf_format(n_clusters=3))
    vl.reconfigure(_ivf_rt(nprobe=3))
    assert vl.count() == 15
    # Each self-query's top-1 is itself -> metadata byte-identical.
    hits = 0
    for i in range(15):
        res = vl.search_sync(vecs[f"v{i}"], 1)
        if res and res[0].id_str == f"v{i}" and res[0].metadata == metas[f"v{i}"]:
            hits += 1
    assert hits == 15
    vl.close()


def test_migrate_round_trip(tmp_path):
    path = str(tmp_path / "vltest")
    vl = VectorLayer.open_separate(path, "test", _flat_format(), Runtime(sync_only=1))
    stored = _insert_clustered(vl)
    n = vl.count()
    # FLAT exact top-1 for a known vector.
    probe_id = "v10"
    probe_vec = stored[probe_id]
    baseline = vl.search_sync(probe_vec, 1)
    assert baseline[0].id_str == probe_id
    set_quiet(True)
    vl.migrate(_ivf_format(n_clusters=3))
    assert vl.count() == n
    vl.migrate(_slsh_format())
    assert vl.count() == n
    vl.migrate(_flat_format())
    assert vl.count() == n
    assert vl.get_format().index_type == IndexType.FLAT
    # Back on FLAT (exact): top-1 unchanged from baseline.
    final = vl.search_sync(probe_vec, 1)
    assert final[0].id_str == probe_id
    vl.close()


def test_migrate_writes_format_to_disk(tmp_path):
    path = str(tmp_path / "vltest")
    rt = Runtime(sync_only=1)
    vl = VectorLayer.open_separate(path, "test", _flat_format(), rt)
    _insert_clustered(vl)
    set_quiet(True)
    vl.migrate(_ivf_format(n_clusters=3))
    vl.close()
    # Disk now says ivf: reopen IVF ok, reopen FLAT raises.
    vl2 = VectorLayer.open_separate(path, "test", _ivf_format(n_clusters=3), _ivf_rt())
    assert vl2.get_format().index_type == IndexType.IVF
    vl2.close()
    with pytest.raises(WaveDBError):
        VectorLayer.open_separate(path, "test", _flat_format(), rt)


def test_migrate_rerun_converges(tmp_path):
    path = str(tmp_path / "vltest")
    vl = VectorLayer.open_separate(path, "test", _flat_format(), Runtime(sync_only=1))
    _insert_clustered(vl)
    n = vl.count()
    set_quiet(True)
    ivf = _ivf_format(n_clusters=3)
    vl.migrate(ivf)
    vl.reconfigure(_ivf_rt(nprobe=3))
    res1 = vl.search_sync([10.0, 0.0, 0.0, 0.0], 5)
    ids1 = {r.id_str for r in res1}
    # Re-run migrate(IVF) -> converges; count + top-5 set unchanged.
    vl.migrate(ivf)
    vl.reconfigure(_ivf_rt(nprobe=3))
    assert vl.count() == n
    res2 = vl.search_sync([10.0, 0.0, 0.0, 0.0], 5)
    ids2 = {r.id_str for r in res2}
    assert ids1 == ids2
    vl.close()


def test_migrate_emits_progress(tmp_path):
    path = str(tmp_path / "vltest")
    vl = VectorLayer.open_separate(path, "test", _ivf_format(n_clusters=3), _ivf_rt())
    _insert_clustered(vl)
    vl.train()
    n = vl.count()
    events = []  # (phase, current, total)
    set_progress_handler(lambda phase, cur, tot: events.append((phase, cur, tot)))
    set_quiet(True)
    # IVF -> SLSH exercises delete (IVF aux) + train (SLSH proj) + rebuild + done.
    vl.migrate(_slsh_format())
    phases = {e[0] for e in events}
    assert "done" in phases
    assert "delete" in phases
    assert "train" in phases
    assert "rebuild" in phases
    # Within rebuild, current is non-decreasing.
    rebuild_cur = [cur for (ph, cur, tot) in events if ph == "rebuild"]
    assert rebuild_cur == sorted(rebuild_cur)
    # done's current == total == count.
    done = [e for e in events if e[0] == "done"]
    assert done and done[-1][1] == done[-1][2] == n
    assert vl.count() == n
    vl.close()


def test_set_log_callback_reregister_safe(tmp_path):
    path = str(tmp_path / "vltest")
    vl = VectorLayer.open_separate(path, "test", _flat_format(), Runtime(sync_only=1))
    _insert_clustered(vl)
    fired = {"first": 0, "second": 0}
    set_quiet(True)

    def first(lvl, msg):
        fired["first"] += 1

    def second(lvl, msg):
        fired["second"] += 1

    set_log_callback(first)
    set_log_callback(second)  # disarms first; only second must fire
    vl.migrate(_ivf_format(n_clusters=3))
    assert fired["first"] == 0
    assert fired["second"] > 0
    # Old cffi callback stayed alive (no UAF); re-registration did not crash.
    vl.close()