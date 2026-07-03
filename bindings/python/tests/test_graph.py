import pytest
from wavedb import WaveDB, GraphLayer


@pytest.fixture
def graph(db_path):
    db = WaveDB(str(db_path))
    g = GraphLayer("graph", db)
    g.insert_sync("alice", "knows", "bob")
    g.insert_sync("bob", "knows", "carol")
    yield g, db
    g.close()
    db.close()


def test_query_vertex(graph):
    g, _ = graph
    result = g.query().vertex("alice").execute_sync()
    assert "alice" in result.vertices


def test_query_out(graph):
    g, _ = graph
    result = g.query().vertex("alice").out("knows").execute_sync()
    assert "bob" in result.vertices


def test_query_out_two_hops(graph):
    g, _ = graph
    result = (
        g.query()
        .vertex("alice")
        .out("knows")
        .out("knows")
        .execute_sync()
    )
    assert "carol" in result.vertices


def test_query_has(graph):
    g, _ = graph
    g.insert_sync("alice", "age", "30")
    result = g.query().vertex("alice").has("age", "30").execute_sync()
    assert "alice" in result.vertices


def test_query_limit(graph):
    g, _ = graph
    g.insert_sync("alice", "knows", "dave")
    result = g.query().vertex("alice").out("knows").limit(1).execute_sync()
    assert len(result.vertices) <= 1


# ── expand_triple: cross-subtree atomic batches ──


def test_expand_triple_shape(db_path):
    """expand_triple returns one canonical root-namespace op per graph index."""
    db = WaveDB(str(db_path))
    g = GraphLayer("graph", db)
    ops = g.expand_triple("alice", "knows", "bob")
    assert len(ops) == 4
    assert all(op["type"] == "put" for op in ops)
    assert all(op["value"] == "" for op in ops)
    # Keys live in the ROOT db namespace: subtree prefix + delimiter + the
    # index key → "graph/spo/..." (single delimiter; the leading slash from
    # the graph key builder is stripped by the C helper). Pass these to
    # WaveDB.batch_sync, NOT to a subtree batch (which would re-prefix).
    assert all(op["key"].startswith("graph/") for op in ops)
    keys = {op["key"] for op in ops}
    assert "graph/spo/alice/knows/bob" in keys
    g.close()
    db.close()


def test_expand_triple_batch_atomic_equivalence(db_path):
    """A triple expanded into a root batch is queryable exactly like insert_sync."""
    db = WaveDB(str(db_path))
    g = GraphLayer("graph", db)
    # One atomic batch: a content write in the root namespace plus a graph
    # triple expanded into root-namespace index ops.
    ops = [
        {"type": "put", "key": "content/ep1/summary", "value": "alice met bob"},
    ] + g.expand_triple("alice", "knows", "bob")
    db.batch_sync(ops)

    assert db.get_sync("content/ep1/summary") == b"alice met bob"
    assert "bob" in g.query().vertex("alice").out("knows").execute_sync().vertices
    g.close()
    db.close()


def test_expand_triple_delete_via_batch(db_path):
    """expand_triple(delete=True) yields del ops that remove the triple."""
    db = WaveDB(str(db_path))
    g = GraphLayer("graph", db)
    g.insert_sync("alice", "knows", "bob")
    assert "bob" in g.query().vertex("alice").out("knows").execute_sync().vertices

    db.batch_sync(g.expand_triple("alice", "knows", "bob", delete=True))
    assert "bob" not in g.query().vertex("alice").out("knows").execute_sync().vertices
    g.close()
    db.close()


def test_expand_triple_stored_keys_match_insert(tmp_path_factory):
    """A triple written via expand_triple+batch stores the exact same index
    keys as insert_sync — the two write paths are fully interchangeable."""
    s, p, o = "alice", "knows", "bob"
    expected = {
        "graph/spo/alice/knows/bob",
        "graph/pos/knows/bob/alice",
        "graph/osp/bob/alice/knows",
        "graph/pso/knows/alice/bob",
    }

    db1 = WaveDB(str(tmp_path_factory.mktemp("ins") / "db"))
    g1 = GraphLayer("graph", db1)
    g1.insert_sync(s, p, o)
    insert_keys = {k for k, _ in db1.create_read_stream(start="graph/")}
    g1.close()
    db1.close()

    db2 = WaveDB(str(tmp_path_factory.mktemp("exp") / "db"))
    g2 = GraphLayer("graph", db2)
    db2.batch_sync(g2.expand_triple(s, p, o))
    expand_keys = {k for k, _ in db2.create_read_stream(start="graph/")}
    g2.close()
    db2.close()

    assert insert_keys == expand_keys
    assert expected <= insert_keys
