"""VectorLayer: approximate-nearest-neighbour index on top of WaveDB.

Mirrors the GraphLayer wrapper pattern: hold an opaque cffi pointer, call
`lib.*`, encode strings, check rc, map errors, lifecycle in
`__enter__`/`__exit__`/`__del__`.

The C `vector_layer_create` signature is::

    vector_layer_t* vector_layer_create(const char *index_name,
                                         database_t *db,
                                         database_subtree_t *subtree,
                                         vector_layer_config_t *config,
                                         int *error_code);

When `subtree` is non-NULL all keys land under that subtree prefix; the
`db` argument is still required (the subtree shares `db`'s underlying
database). When `subtree` is NULL the keys land directly in `db` under
`{index_name}/...`.

`vector_layer_open_separate` opens a DEDICATED database at `db_location`
(no sharing, no subtree) — mirrors the Hippo document_db split for clean
LRU/WAL tuning.

Config is split into a Format tier (immutable after create) and a Runtime
tier (freely mutable via `reconfigure`). The wrapper exposes these as
`Format` and `Runtime` dataclasses; `_fill_format`/`_fill_runtime` copy
them into the C structs.

Only the sync API is wired (insert/search/delete/train/rebuild/count/
reconfigure). The async variants take a caller-owned `promise_t*` whose
resolve/reject callbacks would need a Python trampoline — deferred to a
follow-up. The cdef entries for the async functions are declared so the
symbols resolve, but `VectorLayer` does not call them.
"""
from __future__ import annotations

from dataclasses import dataclass

from ._errors import map_error
from ._native import ffi, lib
from .exceptions import WaveDBError


def _c_string(b: bytes) -> bytes:
    """Append a NUL terminator so any NUL-free bytes object is a valid C string."""
    return b + b"\x00"


class IndexType:
    """C `vl_index_type_t` enum values."""

    FLAT = 0
    IVF = 1
    SLSH = 2


class Distance:
    """C `vl_distance_t` enum values."""

    L2 = 0
    COSINE = 1
    DOT = 2


@dataclass
class Format:
    """Immutable format tier — set at create time.

    `dim` and `delimiter` are immutable for the life of the index (the stored
    vectors and key-paths depend on them). `index_type` is immutable EXCEPT
    via the explicit `VectorLayer.migrate()` operation, which restructures
    the aux keys in place and updates the persisted `__format` leaf -- it is
    NOT a config flip, and reopening with a different `index_type` now raises.
    `distance`/`ivf_n_clusters`/`slsh_*` may also change via `migrate()`.
    `vector_layer_reconfigure` accepts a Runtime only.
    """

    index_type: int
    dim: int
    delimiter: str = "/"
    distance: int = Distance.COSINE
    ivf_n_clusters: int = 0
    slsh_lsh_tables: int = 0
    slsh_hash_bits: int = 0
    slsh_bucket_width: float = 0.0


@dataclass
class Runtime:
    """Runtime tier — freely mutable via `VectorLayer.reconfigure`."""

    top_k: int = 10
    sync_only: int = 1
    ivf_nprobe: int = 8
    ivf_flat_until: int = 1000
    slsh_scan_radius: int = 200


class VectorResult:
    """One hit from `vector_layer_search_sync`.

    `id` is the raw bytes the caller passed to `insert_sync` (NUL-terminated
    by the C layer; `ffi.string` strips the terminator). `metadata` is the
    raw bytes passed to `insert_sync` (empty if none). `distance` is the
    metric distance (lower = closer for L2; for cosine/dot the C layer
    reports the post-transform value — see src/Layers/vector/vector_distance.c).
    """

    def __init__(self, id: bytes, distance: float, metadata: bytes) -> None:
        self.id = id
        self.distance = distance
        self.metadata = metadata

    @property
    def id_str(self) -> str:
        return self.id.decode("utf-8", errors="replace")

    @property
    def metadata_str(self) -> str:
        return self.metadata.decode("utf-8", errors="replace")

    def __repr__(self) -> str:
        return (
            f"VectorResult(id={self.id_str!r}, distance={self.distance:.4f}, "
            f"metadata={self.metadata!r})"
        )


class VectorLayer:
    """ANN index layered on a WaveDB database (shared) or a dedicated db.

    Two construction modes:

    * `VectorLayer.open_separate(db_location, index_name, fmt, rt)` —
      dedicated database at `db_location`. Clean LRU/WAL tuning, no key
      space sharing. Mirrors the Hippo document_db split.

    * `VectorLayer.open(index_name, db, fmt, rt, subtree=None)` —
      shares `db`'s underlying database. If `subtree` is a `Subtree`
      instance, all keys land under that subtree prefix; otherwise keys
      land directly in `db` under `{index_name}/...`.

    The wrapper holds the opaque `vector_layer_t*` and (for the shared-db
    mode with no caller-supplied subtree) an internally-opened subtree
    reference that is closed in `close()` after `vector_layer_destroy`.
    """

    def __init__(self, ptr) -> None:
        self._ptr = ptr
        self._db = None
        self._subtree = ffi.NULL
        self._owns_subtree = False
        self._closed = False

    # ---- construction ----

    @classmethod
    def open_separate(
        cls,
        db_location: str,
        index_name: str,
        fmt: Format,
        rt: Runtime,
    ) -> "VectorLayer":
        if not index_name:
            raise WaveDBError("VectorLayer index_name must be non-empty")
        cfg = ffi.new("vector_layer_config_t*")
        _fill_format(cfg.format, fmt)
        _fill_runtime(cfg.runtime, rt)
        err = ffi.new("int*")
        loc_b = db_location.encode("utf-8") if db_location else ffi.NULL
        name_b = index_name.encode("utf-8")
        ptr = lib.vector_layer_open_separate(loc_b, name_b, cfg, err)
        if ptr == ffi.NULL:
            raise map_error(err[0], "vector_layer_open_separate failed")
        return cls(ptr)

    @classmethod
    def open(
        cls,
        index_name: str,
        db,
        fmt: Format,
        rt: Runtime,
        subtree=None,
    ) -> "VectorLayer":
        if not index_name:
            raise WaveDBError("VectorLayer index_name must be non-empty")
        db._check_open()
        cfg = ffi.new("vector_layer_config_t*")
        _fill_format(cfg.format, fmt)
        _fill_runtime(cfg.runtime, rt)
        err = ffi.new("int*")
        name_b = index_name.encode("utf-8")
        if subtree is not None:
            st_ptr = subtree._st
        else:
            st_ptr = ffi.NULL
        ptr = lib.vector_layer_create(name_b, db._db, st_ptr, cfg, err)
        if ptr == ffi.NULL:
            raise map_error(err[0], "vector_layer_create failed")
        layer = cls(ptr)
        layer._db = db
        # Register self with the parent WaveDB so close() can refuse to
        # destroy the database while the layer is still open. Stored on
        # the same _open_subtrees set used by Subtree/GraphLayer.
        db._open_subtrees.add(layer)
        return layer

    # ---- sync API ----

    def insert_sync(self, id: str, vec, metadata: bytes = b"") -> int:
        self._check_open()
        if not isinstance(id, str):
            raise TypeError(f"id must be str, got {type(id).__name__}")
        if not isinstance(metadata, (bytes, bytearray)):
            raise TypeError(
                f"metadata must be bytes, got {type(metadata).__name__}"
            )
        meta_b = bytes(metadata)
        vec_list = [float(x) for x in vec]
        vec_arr = ffi.new(f"float[{len(vec_list)}]", vec_list)
        rc = lib.vector_layer_insert_sync(
            self._ptr,
            id.encode("utf-8"),
            vec_arr,
            meta_b,
            len(meta_b),
        )
        if rc < 0:
            raise map_error(rc, "vector_layer_insert_sync failed")
        return rc

    def search_sync(self, query, k: int) -> list[VectorResult]:
        self._check_open()
        q_list = [float(x) for x in query]
        q_arr = ffi.new(f"float[{len(q_list)}]", q_list)
        results_ptr = ffi.new("vl_result_t**")
        n_ptr = ffi.new("int*")
        rc = lib.vector_layer_search_sync(
            self._ptr, q_arr, int(k), results_ptr, n_ptr
        )
        if rc < 0:
            raise map_error(rc, "vector_layer_search_sync failed")
        n = n_ptr[0]
        arr = results_ptr[0]
        out: list[VectorResult] = []
        for i in range(n):
            r = arr[i]
            id_bytes = ffi.string(r.id) if r.id != ffi.NULL else b""
            if r.metadata != ffi.NULL and r.metadata_len > 0:
                meta = ffi.buffer(r.metadata, r.metadata_len)[:]
            else:
                meta = b""
            out.append(VectorResult(id_bytes, float(r.distance), meta))
        lib.vector_layer_free_results(arr, n)
        return out

    def delete_sync(self, id: str) -> int:
        self._check_open()
        if not isinstance(id, str):
            raise TypeError(f"id must be str, got {type(id).__name__}")
        rc = lib.vector_layer_delete_sync(self._ptr, id.encode("utf-8"))
        if rc < 0:
            raise map_error(rc, "vector_layer_delete_sync failed")
        return rc

    def train(self) -> int:
        self._check_open()
        rc = lib.vector_layer_train(self._ptr)
        if rc < 0:
            raise map_error(rc, "vector_layer_train failed")
        return rc

    def rebuild(self) -> int:
        self._check_open()
        rc = lib.vector_layer_rebuild(self._ptr)
        if rc < 0:
            raise map_error(rc, "vector_layer_rebuild failed")
        return rc

    def count(self) -> int:
        self._check_open()
        return int(lib.vector_layer_count(self._ptr))

    def reconfigure(self, rt: Runtime) -> int:
        self._check_open()
        rt_c = ffi.new("vector_layer_runtime_t*")
        _fill_runtime(rt_c[0], rt)
        rc = lib.vector_layer_reconfigure(self._ptr, rt_c)
        if rc < 0:
            raise map_error(rc, "vector_layer_reconfigure failed")
        return rc

    def get_format(self) -> Format:
        """Return the layer's current Format.

        The `index_type` is the value persisted in the on-disk `__format` leaf
        (validated against the caller's type at open time); the remaining
        fields are the caller-supplied values from open (dim/delimiter/distance
        and cluster/slsh params are NOT persisted by WaveDB).
        """
        self._check_open()
        out = ffi.new("vector_layer_format_t*")
        rc = lib.vector_layer_get_format(self._ptr, out)
        if rc < 0:
            raise map_error(rc, "vector_layer_get_format failed")
        delim = out.delimiter
        if isinstance(delim, (bytes, bytearray)):
            delim_str = bytes(delim).decode("utf-8", "replace")
        else:
            delim_str = chr(int(delim))
        return Format(
            index_type=int(out.index_type),
            dim=int(out.dim),
            delimiter=delim_str,
            distance=int(out.distance),
            ivf_n_clusters=int(out.ivf_n_clusters),
            slsh_lsh_tables=int(out.slsh_lsh_tables),
            slsh_hash_bits=int(out.slsh_hash_bits),
            slsh_bucket_width=float(out.slsh_bucket_width),
        )

    def migrate(self, fmt: Format) -> int:
        """Migrate this layer to a new index type in place (EXPLICIT op).

        This is the ONLY way to change the index type. Reopening an existing
        index with a different `index_type` now RAISES (WaveDBError) -- it does
        NOT silently degrade and does NOT migrate; you must call `migrate()`.

        `dim` and `delimiter` must match the current layer (the stored vectors
        and key-paths depend on them); `index_type`, `ivf_n_clusters`,
        `slsh_*`, and `distance` may change. The operation deletes the old
        index-type's aux keys, writes the `__format` leaf to the new type
        (recovery anchor), and trains the new index. Stored vectors, `count`,
        and metadata are preserved.

        Crash safety: NOT atomic. `__format` is the recovery anchor (disk +
        memory written in the same step); a crash leaves the index labeled as
        the target type with partially-built aux and all vectors intact.
        Re-run `migrate(target)` to converge (idempotent). The same-type
        short-circuit is intentionally removed so re-running always completes a
        half-built index; use `train()`/`rebuild()` for a cheaper same-type
        retrain.

        Progress: register `wavedb.set_progress_handler(...)` (and optionally
        `wavedb.set_quiet(True)`) before calling. Events are emitted as
        ``vector\\t{phase}\\t{current}\\t{total}`` (scan/delete/train/rebuild/
        done), per 8000-op chunk.
        """
        self._check_open()
        if not isinstance(fmt, Format):
            raise TypeError(f"fmt must be Format, got {type(fmt).__name__}")
        c_fmt = ffi.new("vector_layer_format_t*")
        _fill_format(c_fmt, fmt)
        rc = lib.vector_layer_migrate(self._ptr, c_fmt)
        if rc < 0:
            raise map_error(rc, "vector_layer_migrate failed")
        return rc

    # ---- lifecycle ----

    def _check_open(self) -> None:
        if self._closed or self._ptr == ffi.NULL:
            raise WaveDBError("VectorLayer has been closed")

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        if self._ptr != ffi.NULL:
            lib.vector_layer_destroy(self._ptr)
            self._ptr = ffi.NULL
        # Drop self from the parent WaveDB's open-handle set so the parent
        # can close cleanly. _unregister_subtree uses set.discard, so this
        # is safe even if the parent is already gone.
        if self._db is not None:
            self._db._unregister_subtree(self)
            self._db = None

    def __enter__(self) -> "VectorLayer":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass


# ---- C struct fillers ----


def _fill_format(c_fmt, fmt: Format) -> None:
    """Copy a Format dataclass into a `vector_layer_format_t` cffi struct."""
    delim = fmt.delimiter
    if isinstance(delim, str):
        delim_b = delim.encode("utf-8")
        if len(delim_b) != 1:
            raise WaveDBError(
                f"Format.delimiter must encode to a single byte, got {delim!r}"
            )
        delim_char = delim_b
    elif isinstance(delim, int):
        if not 0 <= delim <= 255:
            raise WaveDBError(
                f"Format.delimiter int must be 0..255, got {delim}"
            )
        delim_char = bytes([delim])
    elif isinstance(delim, (bytes, bytearray)):
        if len(delim) != 1:
            raise WaveDBError(
                f"Format.delimiter bytes must be length 1, got {delim!r}"
            )
        delim_char = bytes(delim)
    else:
        raise TypeError(
            f"Format.delimiter must be str, int, or bytes, got {type(delim).__name__}"
        )
    c_fmt.index_type = int(fmt.index_type)
    c_fmt.dim = int(fmt.dim)
    c_fmt.delimiter = delim_char
    c_fmt.distance = int(fmt.distance)
    c_fmt.ivf_n_clusters = int(fmt.ivf_n_clusters)
    c_fmt.slsh_lsh_tables = int(fmt.slsh_lsh_tables)
    c_fmt.slsh_hash_bits = int(fmt.slsh_hash_bits)
    c_fmt.slsh_bucket_width = float(fmt.slsh_bucket_width)


def _fill_runtime(c_rt, rt: Runtime) -> None:
    """Copy a Runtime dataclass into a `vector_layer_runtime_t` cffi struct."""
    c_rt.top_k = int(rt.top_k)
    c_rt.sync_only = int(rt.sync_only)
    c_rt.ivf_nprobe = int(rt.ivf_nprobe)
    c_rt.ivf_flat_until = int(rt.ivf_flat_until)
    c_rt.slsh_scan_radius = int(rt.slsh_scan_radius)