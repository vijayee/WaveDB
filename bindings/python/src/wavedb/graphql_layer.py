"""GraphQLLayer: GraphQL query interface on top of WaveDB.

The C ``graphql_layer_create`` signature is::

    graphql_layer_t* graphql_layer_create(const char* path,
                                           const graphql_layer_config_t* config,
                                           database_subtree_t* subtree,
                                           int* error_code);

When ``subtree`` is non-NULL the GraphQL layer shares the subtree's
underlying database (``path`` and ``config`` are ignored). When
``subtree`` is NULL the layer creates and owns its own database at
``path`` (or in-memory if ``path`` is NULL).

This binding always uses subtree mode so a ``GraphQLLayer`` shares the
caller's ``WaveDB`` instance. The ``path`` argument to
``GraphQLLayer(path, db)`` is used as the subtree prefix
(e.g. ``"gql"``), giving namespace isolation inside the parent
database. The subtree is opened internally and held for the lifetime of
the layer; ``graphql_layer_destroy`` takes its own reference on the
subtree (via ``refcounter_reference``) and releases it on destroy, so
the binding still owns and closes the original subtree reference.

Result materialization
----------------------

The C API exposes a single result accessor: ``graphql_result_to_json``,
which serializes the result tree to a standard GraphQL JSON response
``{"data": <node>, "errors": [...]}``. The Node and Dart bindings both
use this serializer to materialize results, and we do the same. The
result tree itself (``graphql_result_node_t``) uses a ``vec_t`` template
for children that cannot be declared in cffi ABI mode without a C
compiler, so direct struct walks are not practical.

The ``success`` flag is not included in the JSON output; we infer it
from whether the ``errors`` array is empty (matching the C layer's own
rule: ``result->success = (errors.length == 0)`` after execution).

Default resolver behavior
-------------------------

The default resolver stores entity data under the type's plural path
prefix: ``<plural>/<id>/<field>``. The plural defaults to the type name
plus ``"s"`` (e.g. ``User`` -> ``Users``). Root query fields are matched
by type name or plural name, so ``{ User { name } }`` scans the
``Users/`` prefix and returns a list of entity objects. A field named
``user`` (lowercase) does not match and will not resolve unless the
schema declares it explicitly on the ``Query`` type and a custom
resolver is registered.
"""
from __future__ import annotations

import json

from ._errors import map_error
from ._native import ffi, lib
from .exceptions import WaveDBError


def _c_string(b: bytes) -> bytes:
    """Append a NUL terminator so any NUL-free bytes object is a valid C string."""
    return b + b"\x00"


class GraphQLError:
    """Structured GraphQL error with message and optional path/locations."""

    def __init__(self, message: str, path: str | None = None,
                 locations: list[dict] | None = None) -> None:
        self.message = message
        self.path = path
        self.locations = locations or []

    def __repr__(self) -> str:
        if self.path:
            return f"GraphQLError(message={self.message!r}, path={self.path!r})"
        return f"GraphQLError(message={self.message!r})"


class GraphQLResult:
    """Result of a GraphQL query or mutation.

    The C ``graphql_result_t`` is materialized lazily via
    ``graphql_result_to_json`` on first access to ``data``, ``errors``,
    or ``to_json()``. The JSON buffer is freed immediately after
    parsing; the underlying C result is freed on ``close()``.
    """

    def __init__(self, ptr) -> None:
        self._ptr = ptr
        self._parsed: dict | None = None

    def _ensure_parsed(self) -> dict:
        if self._parsed is not None:
            return self._parsed
        if self._ptr == ffi.NULL:
            self._parsed = {"data": None, "errors": []}
            return self._parsed
        json_ptr = lib.graphql_result_to_json(self._ptr)
        if json_ptr == ffi.NULL:
            self._parsed = {"data": None, "errors": []}
            return self._parsed
        try:
            raw = ffi.string(json_ptr).decode("utf-8", errors="replace")
            parsed = json.loads(raw)
        finally:
            lib.free(json_ptr)
        # The C serializer emits {"data": ..., "errors": [...]} (errors
        # omitted when empty). Normalize so downstream code can rely on
        # both keys being present.
        if "errors" not in parsed:
            parsed["errors"] = []
        self._parsed = parsed
        return self._parsed

    @property
    def data(self):
        """The `data` payload of the GraphQL response (parsed JSON value)."""
        return self._ensure_parsed().get("data")

    @property
    def errors(self) -> list[GraphQLError]:
        """List of `GraphQLError` objects produced by the operation."""
        parsed = self._ensure_parsed()
        out: list[GraphQLError] = []
        for e in parsed.get("errors", []):
            if isinstance(e, dict):
                out.append(GraphQLError(
                    message=e.get("message", "") or "",
                    path=e.get("path"),
                    locations=e.get("locations") or [],
                ))
            else:
                out.append(GraphQLError(message=str(e)))
        return out

    @property
    def success(self) -> bool:
        """True when the result has no errors.

        Matches the C layer's rule: ``result->success = (errors.length == 0)``.
        """
        return len(self.errors) == 0

    def to_json(self) -> str:
        """Return the raw GraphQL JSON response string."""
        if self._ptr == ffi.NULL:
            return json.dumps({"data": None, "errors": []})
        json_ptr = lib.graphql_result_to_json(self._ptr)
        if json_ptr == ffi.NULL:
            return json.dumps({"data": None, "errors": []})
        try:
            return ffi.string(json_ptr).decode("utf-8", errors="replace")
        finally:
            lib.free(json_ptr)

    def close(self) -> None:
        if self._ptr != ffi.NULL:
            lib.graphql_result_destroy(self._ptr)
            self._ptr = ffi.NULL

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass


class GraphQLLayer:
    """GraphQL query/mutation layer on top of a WaveDB database.

    `path` is used as the subtree prefix inside `db` (e.g. ``"gql"``).
    The GraphQL layer shares `db`'s underlying database via a subtree.
    """

    def __init__(self, path: str, db) -> None:
        self._db = db
        self._closed = False
        self._ptr = ffi.NULL
        self._subtree = ffi.NULL

        if not path:
            raise WaveDBError("GraphQLLayer path must be non-empty")
        db._check_open()

        # Open a subtree at `path` on the parent database, then hand it
        # to graphql_layer_create. The layer takes its own reference on
        # the subtree (refcounter_reference in graphql_schema.c), so we
        # still own this reference and must close it ourselves after
        # graphql_layer_destroy.
        prefix_b = path.encode("utf-8")
        prefix_z = _c_string(prefix_b)
        self._subtree = lib.database_subtree_open(
            db._db, ffi.from_buffer(prefix_z), db._delimiter_byte,
        )
        if self._subtree == ffi.NULL:
            raise WaveDBError("database_subtree_open failed for GraphQL layer")

        err = ffi.new("int*")
        # path and config are ignored when subtree is non-NULL; pass NULL.
        self._ptr = lib.graphql_layer_create(ffi.NULL, ffi.NULL, self._subtree, err)
        if self._ptr == ffi.NULL:
            lib.database_subtree_close(self._subtree)
            self._subtree = ffi.NULL
            raise map_error(err[0], "graphql_layer_create failed")

    def schema_parse(self, sdl: str) -> str | None:
        """Parse a GraphQL SDL string and register types.

        Returns ``None`` on success, or the error message string on
        failure. The error string is allocated by the C layer and freed
        here (the C contract is: caller must ``free()`` ``error_out``).
        """
        self._check_open()
        err_out = ffi.new("char**")
        rc = lib.graphql_schema_parse(
            self._ptr, sdl.encode("utf-8"), err_out
        )
        msg: str | None = None
        if rc != 0:
            if err_out[0] != ffi.NULL:
                msg = ffi.string(err_out[0]).decode("utf-8", errors="replace")
                lib.free(err_out[0])
            else:
                msg = f"graphql_schema_parse failed (rc={rc})"
        return msg

    def query_sync(self, query: str) -> GraphQLResult:
        """Execute a GraphQL query synchronously.

        The C layer returns a non-NULL result even on failure (with
        errors populated); NULL indicates an out-of-memory condition.
        """
        self._check_open()
        ptr = lib.graphql_query_sync(self._ptr, query.encode("utf-8"))
        if ptr == ffi.NULL:
            raise WaveDBError("graphql_query_sync returned NULL (out of memory)")
        return GraphQLResult(ptr)

    def _check_open(self) -> None:
        if self._closed or self._ptr == ffi.NULL:
            raise WaveDBError("GraphQLLayer has been closed")

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        if self._ptr != ffi.NULL:
            lib.graphql_layer_destroy(self._ptr)
            self._ptr = ffi.NULL
        # graphql_layer_destroy released the layer's reference on the
        # subtree; close our own reference here.
        if self._subtree != ffi.NULL:
            lib.database_subtree_close(self._subtree)
            self._subtree = ffi.NULL

    def __enter__(self) -> "GraphQLLayer":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass