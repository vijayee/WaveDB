"""Tests for the GraphQLLayer Python binding.

The C GraphQL layer's default resolver stores entity data under the
type's plural path prefix (e.g. ``Users/<id>/<field>``) inside the
layer's subtree namespace. Root query fields are matched by type name
or plural name; supplying an ``id`` argument selects the
``PLAN_BATCH_GET`` path, which resolves each requested entity by exact
path lookup (``<plural>/<id>/<field>``) rather than scanning. Scan-based
queries (no ``id`` argument) are also supported but, in subtree mode,
the C scan iterator returns prefixed paths which confuse the default
resolver's entity-id extraction, so these tests use the id-argument
form which resolves correctly.
"""
import json

import pytest

from wavedb import GraphQLLayer, GraphQLLayerError, WaveDB


@pytest.fixture
def gql(db_path):
    db = WaveDB(str(db_path))
    layer = GraphQLLayer("gql", db)
    schema = """
    type User {
        id: ID!
        name: String
        age: Int
    }
    """
    # schema_parse raises GraphQLLayerError on failure; no return value
    # to check.
    layer.schema_parse(schema)
    # The default resolver looks up fields at <plural>/<id>/<field>,
    # i.e. Users/1/{id,name,age}. The subtree prefix "gql" is applied
    # by the subtree, so we write through the parent db with the full
    # key "gql/Users/1/...".
    db.put_sync("gql/Users/1/id", "1")
    db.put_sync("gql/Users/1/name", "Alice")
    db.put_sync("gql/Users/1/age", "30")
    db.put_sync("gql/Users/2/id", "2")
    db.put_sync("gql/Users/2/name", "Bob")
    yield layer, db
    layer.close()
    db.close()


def test_graphql_query_simple(gql):
    layer, _ = gql
    result = layer.query_sync('{ User(id: "1") { name } }')
    # PLAN_BATCH_GET returns a list of User objects for the requested ids.
    assert result.success
    users = result.data.get("User")
    assert users is not None
    assert len(users) == 1
    user = users[0]
    # The batch-get path auto-includes the entity id; name is resolved
    # from the database.
    assert user.get("id") == "1"
    assert user.get("name") == "Alice"
    result.close()


def test_graphql_query_multiple_fields(gql):
    layer, _ = gql
    result = layer.query_sync('{ User(id: "1") { id name age } }')
    assert result.success
    users = result.data.get("User")
    assert len(users) == 1
    user = users[0]
    assert user.get("id") == "1"
    assert user.get("name") == "Alice"
    assert user.get("age") == 30
    result.close()


def test_graphql_query_alias(gql):
    layer, _ = gql
    result = layer.query_sync('{ admin: User(id: "1") { name } }')
    assert result.success
    admin = result.data.get("admin")
    assert admin is not None
    assert len(admin) == 1
    assert admin[0].get("name") == "Alice"
    result.close()


def test_graphql_query_multi_entity(gql):
    layer, _ = gql
    result = layer.query_sync('{ User(id: "1", id: "2") { name } }')
    assert result.success
    users = result.data.get("User")
    # PLAN_BATCH_GET iterates each id argument; both entities returned.
    assert len(users) == 2
    names = {u.get("name") for u in users}
    assert names == {"Alice", "Bob"}
    result.close()


def test_graphql_query_missing_entity(gql):
    layer, _ = gql
    result = layer.query_sync('{ User(id: "999") { name } }')
    assert result.success
    users = result.data.get("User")
    assert len(users) == 1
    # The entity id is always included; the missing field resolves to null.
    assert users[0].get("id") == "999"
    assert users[0].get("name") is None
    result.close()


def test_graphql_query_invalid_returns_errors(gql):
    layer, _ = gql
    result = layer.query_sync("type !Broken { }")
    assert result.success is False
    assert len(result.errors) >= 1
    assert result.errors[0].message
    result.close()


def test_graphql_schema_parse_error(gql):
    layer, _ = gql
    # schema_parse raises GraphQLLayerError on invalid SDL.
    with pytest.raises(GraphQLLayerError):
        layer.schema_parse("type !Broken { }")


def test_graphql_result_to_json(gql):
    layer, _ = gql
    result = layer.query_sync('{ User(id: "1") { name } }')
    parsed = json.loads(result.to_json())
    assert "data" in parsed
    assert parsed["data"]["User"][0]["name"] == "Alice"
    result.close()


def test_graphql_result_errors_structure(gql):
    layer, _ = gql
    result = layer.query_sync("type !Broken { }")
    err = result.errors[0]
    assert isinstance(err.message, str)
    assert err.message
    result.close()


def test_graphql_layer_context_manager(db_path):
    db = WaveDB(str(db_path))
    with GraphQLLayer("gql2", db) as layer:
        layer.schema_parse("type User { name: String }")
        assert not layer._closed
    assert layer._closed
    db.close()
