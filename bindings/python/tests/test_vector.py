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