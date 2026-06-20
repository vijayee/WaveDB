"""GraphLayer: RDF-style SPO graph on top of WaveDB.

The C `graph_layer_create` signature is::

    graph_layer_t* graph_layer_create(const char* path,
                                       graph_layer_config_t* config,
                                       database_subtree_t* subtree,
                                       int* error_code);

When `subtree` is non-NULL the graph layer shares the subtree's
underlying database (the `path` and `config` arguments are ignored).
When `subtree` is NULL the layer creates and owns its own database at
`path` (or in-memory if `path` is NULL).

This binding always uses the subtree mode so a `GraphLayer` shares the
caller's `WaveDB` instance. The `path` argument to `GraphLayer(path, db)`
is used as the subtree prefix (e.g. ``"graph"``), giving namespace
isolation inside the parent database. The subtree is opened internally
and held for the lifetime of the layer; `graph_layer_destroy` takes its
own reference on the subtree and closes that reference on destroy, so
the binding still owns and closes the original subtree reference.

`graph_result_vertices` returns a count-based `char**` array — the
length comes from `graph_result_count`. It is NOT NULL-terminated.
"""
from __future__ import annotations

from ._errors import map_error
from ._native import ffi, lib
from .exceptions import WaveDBError


def _c_string(b: bytes) -> bytes:
    """Append a NUL terminator so any NUL-free bytes object is a valid C string."""
    return b + b"\x00"


class GraphResult:
    """Result of a graph query execution."""

    def __init__(self, ptr) -> None:
        self._ptr = ptr

    @property
    def vertices(self) -> list[str]:
        """Decode the result's vertex set into a list of Python strings.

        `graph_result_vertices` returns a `const char* const*` array whose
        length is `graph_result_count` (NOT NULL-terminated).
        """
        if self._ptr == ffi.NULL:
            return []
        count = lib.graph_result_count(self._ptr)
        arr = lib.graph_result_vertices(self._ptr)
        if arr == ffi.NULL or count == 0:
            return []
        out = []
        for i in range(count):
            s = arr[i]
            if s != ffi.NULL:
                out.append(ffi.string(s).decode("utf-8", errors="replace"))
            else:
                out.append("")
        return out

    @property
    def count(self) -> int:
        return 0 if self._ptr == ffi.NULL else lib.graph_result_count(self._ptr)

    def close(self) -> None:
        if self._ptr != ffi.NULL:
            lib.graph_result_destroy(self._ptr)
            self._ptr = ffi.NULL

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass


class GraphQuery:
    """Fluent builder for a graph query. Methods return `self` for chaining."""

    def __init__(self, ptr) -> None:
        self._ptr = ptr

    def vertex(self, id: str) -> "GraphQuery":
        rc = lib.graph_query_vertex(self._ptr, id.encode("utf-8"))
        if rc != 0:
            raise map_error(rc, "graph_query_vertex failed")
        return self

    def out(self, predicate: str) -> "GraphQuery":
        rc = lib.graph_query_out(self._ptr, predicate.encode("utf-8"))
        if rc != 0:
            raise map_error(rc, "graph_query_out failed")
        return self

    def in_(self, predicate: str) -> "GraphQuery":
        rc = lib.graph_query_in(self._ptr, predicate.encode("utf-8"))
        if rc != 0:
            raise map_error(rc, "graph_query_in failed")
        return self

    def has(self, predicate: str, value: str) -> "GraphQuery":
        rc = lib.graph_query_has(
            self._ptr, predicate.encode("utf-8"), value.encode("utf-8")
        )
        if rc != 0:
            raise map_error(rc, "graph_query_has failed")
        return self

    def limit(self, n: int) -> "GraphQuery":
        rc = lib.graph_query_limit(self._ptr, n)
        if rc != 0:
            raise map_error(rc, "graph_query_limit failed")
        return self

    def execute_sync(self) -> GraphResult:
        if self._ptr == ffi.NULL:
            raise WaveDBError("GraphQuery already executed or closed")
        ptr = lib.graph_query_execute_sync(self._ptr)
        if ptr == ffi.NULL:
            raise WaveDBError("graph_query_execute_sync returned NULL")
        return GraphResult(ptr)

    def close(self) -> None:
        if self._ptr != ffi.NULL:
            lib.graph_query_destroy(self._ptr)
            self._ptr = ffi.NULL

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass


class GraphLayer:
    """RDF-style SPO graph layered on a WaveDB database.

    `path` is used as the subtree prefix inside `db` (e.g. ``"graph"``).
    The graph layer shares `db`'s underlying database via a subtree.
    """

    def __init__(self, path: str, db) -> None:
        self._db = db
        self._closed = False
        self._ptr = ffi.NULL
        self._subtree = ffi.NULL

        if not path:
            raise WaveDBError("GraphLayer path must be non-empty")
        db._check_open()

        # Open a subtree at `path` on the parent database, then hand it
        # to graph_layer_create. The layer takes its own reference on
        # the subtree (refcounter_reference in graph.c), so we still
        # own this reference and must close it ourselves after
        # graph_layer_destroy.
        prefix_b = path.encode("utf-8")
        prefix_z = _c_string(prefix_b)
        self._subtree = lib.database_subtree_open(
            db._db, ffi.from_buffer(prefix_z), db._delimiter_byte,
        )
        if self._subtree == ffi.NULL:
            raise WaveDBError("database_subtree_open failed for graph layer")

        err = ffi.new("int*")
        # path and config are ignored when subtree is non-NULL; pass NULL.
        self._ptr = lib.graph_layer_create(ffi.NULL, ffi.NULL, self._subtree, err)
        if self._ptr == ffi.NULL:
            lib.database_subtree_close(self._subtree)
            self._subtree = ffi.NULL
            raise map_error(err[0], "graph_layer_create failed")

    def insert_sync(self, s: str, p: str, o: str) -> None:
        self._check_open()
        rc = lib.graph_insert_sync(
            self._ptr, s.encode("utf-8"), p.encode("utf-8"), o.encode("utf-8")
        )
        if rc != 0:
            raise map_error(rc, "graph_insert_sync failed")

    def query(self) -> GraphQuery:
        self._check_open()
        q = lib.graph_query_create(self._ptr)
        if q == ffi.NULL:
            raise WaveDBError("graph_query_create failed")
        return GraphQuery(q)

    def _check_open(self) -> None:
        if self._closed or self._ptr == ffi.NULL:
            raise WaveDBError("GraphLayer has been closed")

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        if self._ptr != ffi.NULL:
            lib.graph_layer_destroy(self._ptr)
            self._ptr = ffi.NULL
        # graph_layer_destroy closed the layer's reference on the
        # subtree; close our own reference here.
        if self._subtree != ffi.NULL:
            lib.database_subtree_close(self._subtree)
            self._subtree = ffi.NULL

    def __enter__(self) -> "GraphLayer":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass
