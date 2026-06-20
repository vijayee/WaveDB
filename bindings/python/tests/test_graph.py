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
