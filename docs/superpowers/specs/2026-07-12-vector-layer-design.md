# WaveDB Vector Layer — Design

Date: 2026-07-12
Status: Approved — implementation pending
Owner: Victor Morrow

## Overview

A new WaveDB **Schema Layer** — `vector_layer` — providing approximate
nearest-neighbor (ANN) vector similarity search over WaveDB's sorted HBTrie,
implemented in C at the library layer and consumed by every binding (Python
cffi, Dart ffi, Node N-API). It sits beside the existing `graph` and
`graphql` layers under `src/Layers/vector/`.

Motivation: consumers currently keep vectors in a FAISS **sidecar**
(`vector_index.faiss` + id/vectors JSON) that lives outside WaveDB — no
persistence, no atomicity, no incremental updates, no MVCC, not in the
`.wavedb` export. An in-engine VectorLayer makes vector storage and semantic
similarity a first-class, durable, atomic part of the database, with no
external dependency and no "rebuild-the-world" index.

Two index types ship: **IVF** (inverted file, native fit for WaveDB's scan
model) and **SK-LSH** (sortable compound LSH keys, now using a bidirectional
scan enabled by a new engine primitive — see Section "Engine: backward scan"
below). Both are runtime-configurable via `vl_index_type_t`. A throwaway-free
benchmark harness (the "spike") validates both clear the recall gate and tunes
the shipped defaults. A FLAT exact-search path is the baseline and IVF's
cold-start fallback.

## Goal

- Vector storage + ANN search as a first-class WaveDB Schema Layer (alongside
  Graph, GraphQL).
- **Pure-C implementation** linking `libwavedb`, using ONLY the exported raw
  byte API (no internal `hbtrie_cursor` symbols — those are not in
  `wavedb.def`). The vector layer uses `database_*_scan*` and
  `database_batch_sync_raw`.
- **Three index types ship**: FLAT (exact), IVF (inverted file), SLSH (SK-LSH).
  All runtime-configurable; their performance parameters runtime-tunable.
- **Sync + async API**, mirroring the Graph layer (`*_sync` + async via
  `Workers/promise.h`).
- **Subtree support**, mirroring `graph_layer_create`'s `database_subtree_t*`
  arg — a vector index can share a root `database_t*`'s key space under a
  subtree prefix, or open a dedicated db.
- Consumed identically by Python (cffi), Dart (ffi), Node (N-API).
- Atomic insert+index via `database_batch_sync_raw` (one WAL_BATCH, one txn).
- A separate `vector_db` instance (or a subtree namespace) so vectors do not
  flush a hot memory store's LRU — mirrors the Hippo `document_db` split.
- **Engine prerequisite**: a new backward scan primitive
  (`database_scan_start_reverse` + `database_scan_prev`) added to the engine
  first as a standalone, strictly-additive commit. SLSH uses it for
  bidirectional neighbor expansion without a dual index.

## Non-goals

- **HNSW** — random-access graph hops need microsecond in-memory latency; on a
  disk-resident forward-scan engine it is persistence-only at best. Out of
  scope.
- **PQ / scalar quantization** — a value-compression layer; can layer on
  later, not in the spike.
- **Replacing consumer extractors (GNN/ontology)** — this is the substrate;
  how a consumer uses it for ontology expansion is a separate design.
- **GPU acceleration** — CPU first; optional SIMD later.
- **In-place format mutation** — the format tier of `vector_layer_config_t`
  (index_type, dim, delimiter, cluster geometry, LSH geometry, distance
  metric) is immutable after create; changing any of it requires dropping the
  subtree (or separate db) and recreating. Enforced structurally (see API).

## Critical constraint: WaveDB was forward-scan only — we add a backward scan

The original engine exposed only **forward range scans** `[start, end)` in
ascending lexicographic order. Verified against
`src/Database/database_iterator.h` / `.c`, `src/HBTrie/hbtrie.h`, and
`src/wavedb.def`:

- Forward scan primitives: `database_scan_start(db, start, end)` +
  `database_scan_next` + `database_scan_end`. Bulk:
  `database_scan_range_sync_raw(db, start, slen, end, elen, delim, &results,
  &count)` (both bounds, forward) and `database_scan_sync_raw` (prefix).
- The cursor (`hbtrie_cursor_t`) is a DFS stack of `(node, entry_index)`
  frames. `hbtrie_cursor_next` increments `entry_index` and descends into the
  leftmost child; there is no `prev`, `reverse`, `seek`, or cursor `clone`.
- B+tree nodes (`bnode_t`) have `level` + sorted `entries`; there are no
  leaf-to-leaf next/prev pointers — leaves are linked only through their
  parent's `entries` array and the DFS stack.
- MVCC visibility (`version_entry_find_visible`) is per-entry and
  direction-agnostic.

**The original design treated "no backward scan" as a law of nature and
worked around it with SLSH's dual index. We reject that.** The DFS stack is
symmetric: a `prev` walk decrements `entry_index` and descends into the
rightmost child; an upper-bound binary search + rightmost descent mirrors the
existing lower-bound `seek_position_leaf` (database_iterator.c:190). MVCC,
vacuum (`open_cursor_count`, `cursor_cvar`), and the iterator frame all work
identically in reverse. So we add a backward scan to the engine as a
strictly-additive, standalone commit BEFORE the vector layer depends on it,
and SLSH simplifies accordingly (no dual index, 2 writes/insert, 1x storage).

See the "Engine: backward scan" section below for the API and tests.

## The three index types + how each maps to scans

Keys use delimiter `/`; a prefix scan is `start = prefix`, `end = prefix +
"\x7f"` (the DEL char as an "everything under prefix" upper bound). For
reverse scans, the same prefix is expressed as `end = prefix + "\x7f"` with
`start = prefix` and walking `database_scan_prev` from just below `end`.

### A. FLAT — exact brute-force (baseline + IVF cold-start)

- Key: `vec/{index}/vector/{id}` -> raw `float[dim]` bytes (+ optional metadata
  suffix).
- `vec/{index}/count` -> int.
- Insert (1 op in one `database_batch_sync_raw`): put vector, increment count.
- Search: `database_scan_range_sync_raw` over `vec/{index}/vector/`,
  brute-force distance in C, top-k. O(N·dim). Recall@10 = 1.0 by definition —
  the baseline the ANN paths measure against.

### B. IVF (Inverted File) — native fit, flat-until-N hybrid

IVF needs no backward scan. Every operation is a forward prefix scan.

**Key layout**
- `vec/{index}/vector/{id}` -> raw `float[dim]` vector bytes (+ optional
  metadata suffix).
- `vec/{index}/centroid/{cid}` -> centroid vector bytes (fixed `cid` space,
  zero-padded for sort order).
- `vec/{index}/cluster/{cid}/{id}` -> membership pointer (value = id or
  empty).
- `vec/{index}/count` -> int (the flat-until-N gate; updated in the insert
  batch).

**Query flow**
1. Prefix-scan `vec/{index}/centroid/` -> all centroids in one forward scan
   (bulk `database_scan_range_sync_raw`, start only).
2. In C, compute `query . centroid` for each, pick top-`nprobe` nearest
   clusters.
3. For each selected `cid`, forward prefix-scan
   `start=vec/{index}/cluster/{cid}/`, `end=vec/{index}/cluster/{cid}/\x7f`
   -> candidate ids.
4. Fetch candidate vectors, **exact rerank** by true distance, return top-k.

**Insert (atomic batch)** — one `database_batch_sync_raw` (3 ops):
- put `vec/{index}/vector/{id}` -> raw vector
- put `vec/{index}/cluster/{cid}/{id}` -> id (membership)
- increment `vec/{index}/count`

**Training** — k-means: assign each vector to nearest centroid, recompute
centroids, repeat. `vector_layer_train` does this. Per-insert assigns to the
current nearest centroid; centroids refresh at train time, not per insert.

**Cold-start hybrid (flat-until-N)** — with `< flat_until` vectors k-means is
degenerate (empty/singleton clusters). Below the threshold, search falls back
to **exact FLAT**: prefix-scan `vec/{index}/vector/`, brute-force distance,
top-k. Above the threshold, switch to IVF. `flat_until` is runtime-tunable.

### C. SK-LSH (SortingKeys-LSH) — bidirectional scan, no dual index

SK-LSH hashes vectors to **sortable compound LSH keys** so that keys close in
sort order correspond to vectors close in distance. Stored sorted, query =
seek to position + **bidirectional expansion** using the new backward scan.

**Key layout**
- `vec/{index}/vector/{id}` -> raw vector.
- `vec/{index}/hash/{lsh_key}/{id}` -> id (forward-ordered index).
- `vec/{index}/proj/{t}` -> projection matrix for LSH table `t`.
- **No `ihash/` subtree** — the dual-index workaround is gone.

**Query flow**
1. Hash query -> `q_key`.
2. Seek to `q_key` in the `hash/` subtree (upper-bound; positions at the
   largest key <= `q_key`, or at the insertion point if absent).
3. Forward `database_scan_next` from there, depth `scan_radius` -> right
   candidates.
4. Backward `database_scan_prev` from the same seek point, depth
   `scan_radius` -> left candidates.
5. Dedup, fetch vectors, **exact rerank**, top-k.

**`slsh_bidirectional` runtime toggle** — 1 (default) = scan both directions
(full recall); 0 = right-only (forward scan only, no `prev`, lower recall,
useful for write-light/read-cheap cases). This replaces the old
`slsh_dual_index` field — semantics changed: no longer about writing a second
index, just about whether the query scans both directions.

**Insert (atomic batch)** — one `database_batch_sync_raw` (2 ops):
- put `vec/{index}/vector/{id}` -> raw vector
- put `vec/{index}/hash/{lsh_key}/{id}` -> id

**Cost vs old dual-index design:** 2 writes/insert (was 3), ~1x storage (was
~2x), one index (was two). Recall should match the old dual-index recall (same
candidate set, no bit inversion); the spike measures this and the gtest gate
(>= 0.90@10) re-asserts it in CI.

**Training** — `vector_layer_train` regenerates projection matrices (random
with fixed seed for reproducibility).

### Comparison

| | FLAT | IVF | SLSH (bidirectional) |
|---|---|---|---|
| Backward scan needed? | no | no | yes (new engine primitive) |
| Recall | 1.0 (exact) | ~0.90-0.95 (tunable nprobe) | ~0.90-0.95 (tunable scan_radius) |
| Storage vs flat | 1x | ~1x + small centroid set | ~1x |
| Writes per insert | 1 (+ count) | 2 (+ count) | 2 |
| Training | none | k-means (periodic) | regen projections |
| Cold-start | n/a (always exact) | degenerate -> needs flat-until-N | fine from 1 vector |
| Exact baseline | is the baseline | flat-until-N gives it free | flat fallback can be added |

## Engine: backward scan (prerequisite, standalone commit)

A strictly-additive engine addition, landed as its own commit BEFORE the
vector layer depends on it. The existing forward path is untouched; existing
forward tests stay green.

**Files touched (existing, no new files):**
- `src/HBTrie/hbtrie.h` — declare `hbtrie_cursor_prev`, and either a direction
  flag on `hbtrie_cursor_t` or a `hbtrie_cursor_init_reverse` initializer.
- `src/HBTrie/hbtrie.c` — implement `hbtrie_cursor_prev` (mirror of `next`:
  decrement `entry_index`, descend rightmost child), and a rightmost-descent
  positioning helper.
- `src/Database/database_iterator.h` — declare `database_scan_start_reverse`
  and `database_scan_prev`. `database_scan_end` is reused.
- `src/Database/database_iterator.c` — implement `seek_position_leaf_reverse`
  (upper-bound binary search + rightmost descent, mirror of
  `seek_position_leaf` at line 190), `database_scan_start_reverse`,
  `database_scan_prev`.
- `src/wavedb.def` — add `database_scan_start_reverse`, `database_scan_prev`.

**API:**

```c
/* Scan keys in descending order. start_path = lower bound (scan stops when
   reached, NULL = no lower bound). end_path = upper bound (scan starts at
   the largest key < end_path, NULL = beginning-of-db = largest key in db).
   Mirror of forward [start, end) reversed to (start, end] walked backward:
   emits keys in descending order, starting just below end_path and stopping
   at start_path. */
database_iterator_t* database_scan_start_reverse(database_t* db,
                                                  path_t* start_path,
                                                  path_t* end_path);
int database_scan_prev(database_iterator_t* iter,
                        path_t** out_path,
                        identifier_t** out_value);
/* iter is freed by existing database_scan_end. */
```

**Vacuum interaction:** reverse cursors increment the same
`db->open_cursor_count` and wait on the same `db->cursor_cvar` as forward
cursors. `database_vacuum_auto` already waits on `cursor_cvar` — reverse
cursors are tracked identically, so vacuum correctness is unchanged.

**Tests** (`tests/test_reverse_scan.cpp`, new gtest):
- `ReverseScanAll` — insert N keys, reverse scan emits all in descending
  order.
- `ReverseScanRange` — `(start, end]` walked backward emits the right slice
  in descending order.
- `ReverseScanPrefix` — reverse prefix scan via the `\x7f` upper bound.
- `ReverseScanMVCC` — versions visible to the read txn are emitted in reverse;
  invisible ones skipped (mirror of forward MVCC test).
- `ReverseScanSubtree` — reverse scan over a db that has a subtree (full
  paths, or a `database_subtree_scan_start_reverse` if the subtree API mirrors
  it — to be decided at implementation time, see Open questions).
- `ForwardScanUnchanged` — existing forward scan tests stay green (regression
  guard).

**Effort**: ~400-600 lines of C + ~200 lines of tests. One commit. Lands
before any vector layer code.

## C API (`src/Layers/vector/vector_layer.h`)

Follows the Graph layer precedent (`graph.h`): opaque struct, `extern "C"`
guards, create/destroy/insert/search/delete/train. Holds a `database_t*`
(either passed in, or opened as a separate `vector_db` instance).

```c
#ifndef WAVEDB_VECTOR_LAYER_H
#define WAVEDB_VECTOR_LAYER_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vector_layer_t vector_layer_t;

typedef enum {
    VL_INDEX_FLAT = 0,   /* exact brute-force; baseline + IVF cold-start */
    VL_INDEX_IVF  = 1,   /* inverted file */
    VL_INDEX_SLSH = 2    /* SK-LSH bidirectional (uses engine backward scan) */
} vl_index_type_t;

typedef enum {
    VL_DIST_L2     = 0,
    VL_DIST_COSINE = 1,   /* default for embedding workloads */
    VL_DIST_DOT    = 2
} vl_distance_t;

/* Format tier — IMMUTABLE after create. To change any field here, drop the
   subtree (or separate db) and recreate the layer. vector_layer_reconfigure
   accepts only the runtime tier; the C signature makes format mutation
   structurally impossible. */
typedef struct {
    vl_index_type_t index_type;
    int      dim;                 /* vector dimensionality */
    char     delimiter;           /* path-segment separator, default '/' */
    vl_distance_t distance;       /* distance metric for assignment + rerank */
    /* IVF format-defining */
    int      ivf_n_clusters;
    /* SLSH format-defining */
    int      slsh_lsh_tables;     /* compound hash width (L) */
    int      slsh_hash_bits;      /* bits per table */
    float    slsh_bucket_width;   /* LSH projection width W */
} vector_layer_format_t;

/* Runtime tier — freely mutable via vector_layer_reconfigure. */
typedef struct {
    int      top_k;               /* default result count */
    int      sync_only;           /* 1 = single-threaded, disable MVCC */
    /* IVF runtime */
    int      ivf_nprobe;
    int      ivf_flat_until;      /* exact FLAT below this many vectors */
    /* SLSH runtime */
    int      slsh_scan_radius;    /* forward+backward scan depth each direction */
    int      slsh_bidirectional;  /* 1 = scan both directions (default);
                                     0 = right-only (lower recall, cheaper) */
} vector_layer_runtime_t;

typedef struct {
    vector_layer_format_t  format;
    vector_layer_runtime_t runtime;
} vector_layer_config_t;

typedef struct {
    char    *id;
    float    distance;
    uint8_t *metadata;            /* caller frees via vector_layer_free_results */
    size_t   metadata_len;
} vl_result_t;

/* Open on an EXISTING database_t (shared key space, e.g. with Graph).
   If subtree is non-NULL, all keys land under that subtree prefix. */
vector_layer_t* vector_layer_create(const char *index_name,
                                     database_t *db,
                                     database_subtree_t *subtree,
                                     vector_layer_config_t *config,
                                     int *error_code);

/* Open a DEDICATED vector_db instance at db_location (clean LRU/WAL tuning,
   mirrors the Hippo document_db split). No subtree. */
vector_layer_t* vector_layer_open_separate(const char *db_location,
                                            const char *index_name,
                                            vector_layer_config_t *config,
                                            int *error_code);

void  vector_layer_destroy(vector_layer_t *vl);

/* Reconfigure runtime-tunable params only. Format tier is immutable after
   create; to change index_type/dim/delimiter/ivf_n_clusters/slsh_*, drop the
   subtree (or separate db) and recreate. */
int   vector_layer_reconfigure(vector_layer_t *vl,
                               vector_layer_runtime_t *runtime);

/* Insert is atomic: vector + index entries in one database_batch_sync_raw. */
int   vector_layer_insert(vector_layer_t *vl, const char *id,
                          const float *vec,
                          const uint8_t *metadata, size_t metadata_len);   /* async */
int   vector_layer_insert_sync(vector_layer_t *vl, const char *id,
                               const float *vec,
                               const uint8_t *metadata, size_t metadata_len);

int   vector_layer_insert_batch(vector_layer_t *vl,
                                const char **ids, const float **vecs,
                                const uint8_t **metadatas, const size_t *meta_lens,
                                int n);                                       /* async */
int   vector_layer_insert_batch_sync(vector_layer_t *vl,
                                     const char **ids, const float **vecs,
                                     const uint8_t **metadatas, const size_t *meta_lens,
                                     int n);

int   vector_layer_search(vector_layer_t *vl, const float *query, int k,
                          vl_result_t **results, int *n_results);            /* async */
int   vector_layer_search_sync(vector_layer_t *vl, const float *query, int k,
                               vl_result_t **results, int *n_results);

int   vector_layer_delete(vector_layer_t *vl, const char *id);               /* async */
int   vector_layer_delete_sync(vector_layer_t *vl, const char *id);

/* train: IVF (re)compute centroids (k-means); SLSH (re)gen projections;
          FLAT no-op. Sync-only (caller off-threads if needed). */
int   vector_layer_train(vector_layer_t *vl);

/* rebuild: rewrite the index's per-vector entries (IVF memberships, SLSH
   hashes) from the stored vectors. Preserves vec/{index}/vector/*. Sync-only. */
int   vector_layer_rebuild(vector_layer_t *vl);

size_t vector_layer_count(vector_layer_t *vl);
void   vector_layer_free_results(vl_result_t *results, int n);

#ifdef __cplusplus
}
#endif
#endif
```

Every public function translates to `database_*_sync_raw` /
`database_batch_sync_raw` / `database_scan_*` against the layer's
`database_t*`. The layer never touches `hbtrie_cursor_*` (un-exported). The
exact-flat path (`VL_INDEX_FLAT` / IVF below `flat_until`) is a single
`database_scan_range_sync_raw` over `vec/{index}/vector/` + in-C brute force.
SLSH uses `database_scan_start_reverse` + `database_scan_prev` for left
neighbor expansion.

## File layout

```
src/Layers/vector/
  vector_layer.h          # public API, extern "C", opaque struct
  vector_layer.c          # lifecycle, config, dispatch by index_type
  vector_flat.c           # exact brute-force (baseline + IVF cold-start)
  vector_ivf.c            # inverted file
  vector_slsh.c           # SK-LSH bidirectional (uses engine backward scan)
  vector_internal.h       # shared: distance funcs, key encoding, raw_op helpers
  vector_distance.c       # L2 / cosine / dot, SIMD-optional
bench/vector/
  bench_vector.c          # C bench driver, linked to wavedb_static
  corpus/                 # generated embeddings (.fvec) + ground truth (.gt), gitignored
  scripts/embed_corpora.py  # sentence-transformers: produce .fvec from synthetic
                            # + EnterpriseRAG-Bench subset
  REPORT.md               # spike results, committed
tests/
  test_vector.cpp         # gtest, C-level
  test_reverse_scan.cpp   # gtest, engine backward scan (lands first, own commit)
bindings/python/src/wavedb/vector_layer.py
bindings/dart/lib/vector_layer.dart + lookupFunction entries in wavedb_bindings.dart
bindings/nodejs/c_src/vector_layer.cc + binding.gyp entry
```

## Build + bindings wiring

Following the Graph layer precedent end-to-end.

**C / CMake** (root `CMakeLists.txt`):
1. Create `src/Layers/vector/vector_layer.c` + `vector_layer.h` (`extern
   "C"` guards, opaque struct, the API above), plus `vector_flat.c`,
   `vector_ivf.c`, `vector_slsh.c`, `vector_distance.c`, `vector_internal.h`.
2. Add the `src/Layers/vector/*.c` files to `WAVEDB_SOURCES` after the Graph
   block (~`:131`). This auto-builds them into BOTH `wavedb` (static) and
   `wavedb_shared` (FFI shared -> `libwavedb.so/.dll/.dylib`). No new engine
   source files are added — the backward scan is implemented in existing
   `src/HBTrie/hbtrie.c` and `src/Database/database_iterator.c`.
3. Add every exported `vector_layer_*` symbol to `src/wavedb.def` (MSVC DLL
   export — this IS the FFI surface). Also add the two new engine symbols
   `database_scan_start_reverse`, `database_scan_prev`.

**C test** (`tests/test_vector.cpp` + `tests/test_reverse_scan.cpp`,
GoogleTest):
- `add_executable(test_vector tests/test_vector.cpp)`;
  `target_link_libraries(test_vector wavedb gtest gtest_main)`;
  `add_test(NAME test_vector COMMAND test_vector)`.
- `add_executable(test_reverse_scan tests/test_reverse_scan.cpp)`;
  `target_link_libraries(test_reverse_scan wavedb gtest gtest_main)`;
  `add_test(NAME test_reverse_scan COMMAND test_reverse_scan)`.
- Both mirror the Graph test block (`CMakeLists.txt:400-402`).

**C bench**:
- `add_executable(bench_vector bench/vector/bench_vector.c)`;
  `target_link_libraries(bench_vector wavedb)`. Not registered with `add_test`
  — it's a bench, not a unit test. `make corpus` invokes the Python embedding
  script once; after that the bench reads cached `.fvec` files.

**Python bindings** (cffi, out-of-line + ABI fallback):
4. Add `vector_layer_*` cdef entries to BOTH `_cffi_build.py` (compiled) and
   `_native_abi.py` (ABI fallback) — they must stay in sync (the codebase
   duplicates them verbatim). Add `typedef struct vector_layer_t
   vector_layer_t;` to the types cdef block in both, and to the `_C_SOURCE`
   forward-declaration preamble in `_cffi_build.py`.
5. Add `src/wavedb/vector_layer.py` wrapper (mirror `graph_layer.py`: hold
   the opaque `ffi` pointer, call `lib.vector_layer_*`, encode strings, check
   rc, `map_error`, wrap lifecycle in `__enter__/__exit__/__del__`). Config
   as a Python dataclass with `Format` / `Runtime` nested classes mirroring
   the C split — `__init__` validates format fields are only set at
   construction; `reconfigure` accepts only a `Runtime` instance. Exposes
   `open` / `open_separate` classmethods, `insert` / `insert_sync` /
   `search` / `search_sync` / `delete` / `delete_sync` / `train` / `rebuild`
   / `reconfigure` / `count` / `free_results`.
6. Export `VectorLayer` from `src/wavedb/__init__.py`.
7. `tests/test_vector.py` (pytest, reuse the `db_path` fixture from
   `conftest.py`): same coverage as gtest, including async path via existing
   promise machinery, recall gates asserted, error mapping via `map_error`.
8. Re-run `bindings/python/scripts/copy_sources.py` before building an sdist
   so `c_src/` reflects the new `src/Layers/vector/` files.

**Dart** (`bindings/dart/`): add `lookupFunction<...C, ...Dart>('symbol')`
entries to `wavedb_bindings.dart` for every `vector_layer_*` symbol, plus a
`vector_layer.dart` wrapper mirroring `graph_layer.dart`. Dart is async-first
— `insert`/`search`/`delete` return `Future`, `*_sync` return values
directly. Config as Dart classes with `Format`/`Runtime` nested, format
immutable after construct. Tests in `bindings/dart/test/test_vector.dart`,
same coverage as pytest. Consumes the same `wavedb_shared` target.

**Node** (`bindings/nodejs/`): add `vector_layer.cc` N-API wrapper in
`bindings/nodejs/c_src/`, add it to `binding.gyp` (and the nodejs
`CMakeLists.txt` if it builds the C wrapper via CMake). Node is async-first —
`insert`/`search`/`delete` return `Promise`, `*_sync` return values. Config as
JS objects; the wrapper validates format fields aren't mutated after open.
Tests in `bindings/nodejs/test/test_vector.js`, same coverage as pytest, using
whatever the existing Graph test uses (mocha or node:test). Exports
`VectorLayer` from `bindings/nodejs/lib/`.

**Cross-binding consistency**: all three expose the same `Format`/`Runtime`
split with the same immutability rule; all three assert the same recall gates
in their tests; all three handle `vl_result_t` → host-language result
conversion the same way (id string, distance number, metadata bytes/Buffer).

No `setup.py` / `pyproject.toml` changes are required — the build pipeline is
generic (it globs the C tree and the cdef list).

## Sequencing (C-first)

Phase 0 — Engine: backward scan (standalone commit, lands first):
- `hbtrie_cursor_prev`, `seek_position_leaf_reverse`,
  `database_scan_start_reverse`, `database_scan_prev`.
- `tests/test_reverse_scan.cpp` with the tests listed in the Engine section.
- Forward path untouched; existing forward tests stay green.

Phase 1 — C layer (FLAT → IVF → SLSH, production-quality):
- `vector_distance.c`, `vector_internal.h`, then `vector_flat.c`, `vector_ivf.c`,
  `vector_slsh.c`, `vector_layer.c`, `vector_layer.h`.
- `tests/test_vector.cpp` extended incrementally per index.
- All 17 `vector_layer_*` symbols in `src/wavedb.def` from day one.

Phase 2 — Spike / benchmark harness:
- `bench/vector/scripts/embed_corpora.py` generates the hybrid corpus (synthetic
  + EnterpriseRAG-Bench subset).
- `bench/vector/bench_vector.c` runs the search arms, writes
  `bench/vector/REPORT.md`.
- Decision gate applied: both IVF and SLSH must clear `recall >= 0.90@10` at
  the configured params. Failure raises `flat_until` / tightens `scan_radius`
  and documents the failure — both ship regardless.
- Spike output tunes the defaults in `vector_layer.h`'s
  `vector_layer_config_t` initializers.

Phase 3 — Bindings (against frozen C API):
- Python (cffi) first — most precedent.
- Dart (ffi) — mirror.
- Node (N-API) — mirror.
- Each binding's tests assert the same recall gates.

Phase 4 — De-wonk + finish.

## Spike / benchmark harness

Since both indexes ship, the spike's job is to **validate both clear the
recall gate and produce the tuning numbers baked into the shipped defaults.**
Nothing is throwaway — the bench harness ships with the layer.

**Data prep** (`bench/vector/scripts/embed_corpora.py`, Python):
- *Synthetic arms*: numpy generates 10k / 30k / 50k vectors at 384 / 768 /
  1536-dim, two distributions — `gaussian` (iid N(0,1)) and `clustered_blobs`
  (50 gaussians, σ=0.1, well-separated centers). Brute-force ground truth
  computed in numpy (top-10 NN per query for 500 random query vectors per arm).
- *Real arm*: download a 10-30k slice from one EnterpriseRAG-Bench source
  (recommend Linear or Confluence — small, clean text), embed with
  `bge-small-en-v1.5` via `sentence-transformers` (384-dim, ~100MB model). 500
  query vectors sampled from the same source. Brute-force ground truth.
- Output: `bench/vector/corpus/{name}.fvec` (raw `float[dim]` row-major, header
  = `{dim, count, query_count}`) + `{name}.gt` (top-10 ids per query). Pure
  binary, no Python needed to read.

**Bench driver** (`bench/vector/bench_vector.c`, linked to `wavedb`):
- Args: corpus name, index type (FLAT/IVF/SLSH), runtime params (nprobe /
  scan_radius / flat_until / bidirectional), k, runs.
- Per run: load corpus, create layer (open_separate at temp path), insert all
  vectors (timed), optionally `train` (IVF/SLSH), run all 500 queries (timed,
  per-query latency recorded), measure storage (page-file size), free.
- Output to stdout: `recall@10` (vs ground truth), `p50`/`p99` search latency,
  insert throughput (ops/sec), storage bytes/vector, plus a one-line JSON
  summary per run for aggregation.

**Decision gate** — both indexes ship, so the gate validates each is
acceptable, not which wins:

| Index | Gate | Failure action |
|---|---|---|
| FLAT | recall@10 == 1.0 (by definition) | n/a — baseline |
| IVF | recall@10 >= 0.90 @ nprobe <= 8 on 50k x 384 | Raise `flat_until` default, document the failure, ship with a higher default threshold; do NOT drop IVF. |
| SLSH bidirectional | recall@10 >= 0.90 @ scan_radius in doc's range | Tighten default `scan_radius`, document; ship regardless. |
| SLSH right-only (`slsh_bidirectional=0`) | recall@10 >= 0.80 (looser, documents the bidirectional cost) | Default `slsh_bidirectional=1`, document right-only as a write-light escape hatch only. |

The spike's output is `bench/vector/REPORT.md` with the table per arm, plus
the `vector_layer_config_t` defaults in `vector_layer.h` updated to the
spike's tuning numbers. The gtest recall gates re-assert these in CI.

**Corpus caching**: `.fvec` files generated once via `make corpus` (invokes
the Python script), then cached. Not regenerated every run. Gitignored (large).

## Test plan

**Engine** (`tests/test_reverse_scan.cpp`, gtest) — listed in the Engine
section above.

**Vector layer** (`tests/test_vector.cpp`, gtest):
- *Lifecycle*: `CreateShared`, `CreateSeparate`, `CreateSubtree`,
  `ReconfigureRuntime`, `ReconfigureFormatRejected` (asserts format tier read
  back matches create after a reconfigure — documents immutability at test
  level).
- *FLAT*: `FlatInsertSearch`, `FlatExactMatchesBruteForce` (recall@10 == 1.0
  — baseline), `FlatMetadata`, `FlatCount`.
- *IVF*: `IVFInsertSearch` (recall@10 >= 0.90 vs brute force — gate asserted
  in-test), `IVFFlatUntilNSwitch` (FLAT path below threshold, IVF path
  above), `IVFTrainRebuild` (centroids change on train; memberships rewritten
  on rebuild).
- *SLSH*: `SLSHInsertSearchBidirectional` (recall@10 >= 0.90),
  `SLSHInsertSearchRightOnly` (recall@10 >= 0.80 — looser gate documents the
  bidirectional cost), `SLSHRebuild` (hashes rewritten).
- *Async*: `AsyncInsertSearch`, `AsyncBatchInsert` (all land atomically;
  forced mid-batch failure leaves no partial state).
- *Atomicity*: `AtomicBatchRollback` (mid-batch failure leaves no orphan
  `cluster/` / `hash/` keys; `count` unchanged).
- *Regression*: existing Graph/GraphQL/subtree suites stay green (the vector
  layer is purely additive; no shared symbols touched).

**Bindings**: `bindings/python/tests/test_vector.py` (pytest) — same surface
via the `VectorLayer` wrapper; round-trip metadata; error mapping via
`map_error`. `bindings/dart/test/test_vector.dart` and
`bindings/nodejs/test/test_vector.js` — same coverage.

## Honest caveats / risks

- **R1 backward-scan** — REMOVED as a constraint. We built the backward scan.
- **R2 IVF cold-start**: degenerate clusters below the `flat_until`
  threshold; the hybrid is mandatory, not optional. Threshold is runtime-tunable.
- **R3 recall is approximate**: both IVF and SLSH lose recall; exact-flat is
  the baseline AND the cold-start fallback. The gtest recall gates assert the
  doc's gate in CI. Never claim exact recall for the ANN paths.
- **R4 embedding quality (out of scope here)**: any downstream win that
  consumes the layer depends on embeddings capturing relational/functional
  similarity. The consumer's concern, not the VectorLayer's.
- **R5 atomicity unit = one batch.** No explicit begin/commit/rollback; a
  failed `database_batch_sync_raw` leaves no partial writes (WAL_BATCH is
  atomic), but the layer must not issue index puts outside a batch.
- **R6 subtrees vs separate db**: both supported at the API. Separate db for
  clean LRU/WAL; subtree for shared-key-space use (e.g. with Graph on the same
  root db).
- **R7 MSVC export discipline**: forgetting a symbol in `wavedb.def` means it
  resolves at build on GCC/Clang but fails on MSVC. Every `vector_layer_*` and
  the two new `database_scan_*_reverse` / `database_scan_prev` symbols go in
  `wavedb.def` from day one.
- **R8 cdef sync**: `_cffi_build.py` and `_native_abi.py` are hand-duplicated;
  drifting them silently breaks the ABI fallback. Treat them as one list.
- **R9 both-indexes-ship surface area**: shipping FLAT + IVF + SLSH +
  async/sync + subtrees triples the test matrix vs a winner-only plan.
  Mitigation: gtest recall gates + per-binding test parity so each path is
  CI-asserted.
- **R10 format/runtime split is a new API contract**: the immutability rule
  is structural (reconfigure signature takes only `runtime`), but bindings
  must mirror it — a binding that lets users mutate format fields after open
  silently corrupts the index. Mitigation: each binding's wrapper validates
  format-immutability at the wrapper layer.
- **R11 Python data-prep dependency**: the spike's real arm needs
  `sentence-transformers` + `bge-small-en-v1.5` (~100MB) + internet to
  download the EnterpriseRAG-Bench slice. Mitigation: corpus generated once
  and cached; `make corpus` is a separate target; the bench runs offline
  after that. Synthetic arms have no external deps.
- **R12 bench-driver is C, not Python**: the doc allowed "pytest or a small C
  bench." C-first means C, which is faster but less flexible for ad-hoc
  exploration. Mitigation: the bench takes runtime params as CLI args, so
  exploration is via re-running with different args.
- **R13 engine regression**: the reverse scan touches `HBTrie/` and
  `Database/` engine files. Mitigation: strictly additive (new functions,
  existing forward path unchanged); `ForwardScanUnchanged` test guards the
  forward path; reverse lands as its own commit before the vector layer.
- **R14 SLSH correctness depends on the new reverse scan**: if the reverse
  scan has a bug, SLSH recall drops silently. Mitigation: reverse scan has
  its own gtest suite before the vector layer depends on it; SLSH's recall
  gate re-asserts end-to-end.

## Open questions for the spike to resolve

- Does IVF clear `recall >= 0.90@10` at `nprobe <= 8` on 50k x 384, and is the
  centroid-scan cost acceptable as cluster count grows? (Sets default `nprobe`.)
- Does SLSH bidirectional clear the gate at a `scan_radius` that's cheap
  enough? (Sets default `scan_radius`.)
- Does SLSH right-only (`slsh_bidirectional=0`) clear 0.80 at acceptable
  latency? (Decides whether right-only is a documented escape hatch or gets
  dropped from defaults.)
- What `flat_until` keeps cold-start exact without dominating memory at
  scale? (Sets default `flat_until`.)
- 384 vs 768 vs 1536: does recall degrade enough at higher dim to force a
  dimensionality-reduction step (PCA/Hilbert) before hashing — a third index
  arm the spike should leave room for?
- Do synthetic and real arms agree on which index is better? If they
  disagree, the real arm wins for default tuning; synthetic stays as a
  regression baseline.
- Does `database_subtree_scan_start_reverse` need to be added alongside
  `database_scan_start_reverse` for the subtree path, or do reverse scans
  on a subtree'd db just use full paths? Resolved at Phase 0 implementation
  time based on how `database_subtree_scan_start` is structured.

## Status / next

Design approved. The implementation plan lands at
`docs/superpowers/plans/2026-07-12-vector-layer.md` (Phase/Task/Step template)
via the writing-plans skill, covering Phase 0 (engine backward scan) through
Phase 4 (de-wonk + finish). De-wonk runs at the completion gate of the
implementation, not now.