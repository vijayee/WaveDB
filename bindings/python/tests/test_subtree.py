import pytest
from wavedb import WaveDB


def test_subtree_scoped_ops(db_path):
    db = WaveDB(str(db_path))
    st = db.open_subtree("users")
    st.put_sync("alice/name", "Alice")
    st.put_sync("bob/name", "Bob")
    assert st.get_sync("alice/name") == b"Alice"
    assert db.get_sync("users/alice/name") == b"Alice"
    st.close()
    db.close()


def test_subtree_delete_prefix(db_path):
    db = WaveDB(str(db_path))
    db.put_sync("users/alice", "1")
    db.put_sync("users/bob", "2")
    db.put_sync("other/x", "y")
    db.delete_subtree("users")
    assert db.get_sync("users/alice") is None
    assert db.get_sync("users/bob") is None
    assert db.get_sync("other/x") == b"y"
    db.close()


@pytest.mark.asyncio
async def test_subtree_async(db_path):
    db = WaveDB(str(db_path))
    st = db.open_subtree("users")
    await st.put("alice/name", "Alice")
    assert await st.get("alice/name") == b"Alice"
    st.close()
    await db.aclose()


def test_subtree_context_manager(db_path):
    db = WaveDB(str(db_path))
    with db.open_subtree("s") as st:
        st.put_sync("k", "v")
        assert st.get_sync("k") == b"v"
    db.close()