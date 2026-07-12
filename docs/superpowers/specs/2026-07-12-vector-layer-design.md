# WaveDB Vector Layer — Design

Date: 2026-07-12
Status: Draft (pending spike)
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

Two index types are identified as viable and will be settled by a throwaway
**spike**: **IVF** (inverted file, native fit for WaveDB's scan model) and
**SK-LSH** (sortable compound LSH keys, requiring a dual-index workaround
because WaveDB cannot scan backward). The spike picks one (or a hybrid) for
the full build.

## Goal

- Vector storage + ANN search as a first-class WaveDB Schema Layer (alongside
  Graph, GraphQL).
- **Pure-C implementation** linking `libwavedb`, using ONLY the exported raw
  byte API (no internal `hbtrie_cursor` symbols — those are not in
  `wavedb.def`).
- Two candidate index types evaluated by a throwaway spike; pick one (or a
  flat-until-N hybrid) for the full build.
- Consumed identically by Python (cffi), Dart (ffi), Node (N-API).
- Atomic insert+index via `database_batch_sync_raw` (one WAL_BATCH, one txn).
- A separate `vector_db` instance (or a subtree namespace) so vectors do not
  flush a hot memory store's LRU — mirrors the Hippo `document_db` split.

## Non-goals

- **HNSW** — random-access graph hops need microsecond in-memory latency; on a
  disk-resident forward-scan engine it is persistence-only at best. Out of
  scope.
- **PQ / scalar quantization** — a value-compression layer; can layer on
  later, not in the spike.
- **Replacing consumer extractors (GNN/ontology)** — this is the substrate;
  how a consumer uses it for ontology expansion is a separate design.
- **GPU acceleration** — CPU first; optional SIMD later.
- **Adding backward/reverse scan to WaveDB itself** — we design AROUND the
  forward-only constraint; we do not modify the engine's iterator.

## Critical constraint: WaveDB is forward-scan only

Verified against `src/Database/database_iterator.h` / `.c`,
`src/HBTrie/hbtrie.h`, and `src/wavedb.def`:

- The ONLY scan primitive is a **forward range scan `[start, end)`**, ascending
  lexicographic order. Streaming: `database_scan_start(db, start, end)` +
  `database_scan_next` + `database_scan_end`. Bulk:
  `database_scan_range_sync_raw(db, start, slen, end, elen, delim, &results,
  &count)` (both bounds, forward) and `database_scan_sync_raw` (prefix).
- There is **NO `prev`, NO `reverse`, NO `seek`, NO cursor `clone`.** The
  low-level `hbtrie_cursor_t.path` arg is documented "future seek support
  (currently unused)"; `hbtrie_cursor_*` is not even in `wavedb.def`, so it is
  not linkable.
- DB handle is **`database_t*`** (not `wdb_t`).
- Atomic write is **`database_batch_sync_raw(db, delimiter, raw_op_t* ops,
  count)`** — one WAL_BATCH, one txn ID. `raw_op_t{key, key_len, value,
  value_len, type}` where `type` 0=put, 1=delete. This is the "insert vector +
  index entries atomically" primitive.
- Raw byte API: `database_put_sync_raw`, `database_get_sync_raw` (-2 = not
  found), `database_delete_sync_raw`, `database_raw_value_free`; results
  `raw_result_t{key, key_len, value, value_len}` freed via
  `database_raw_results_free`.
- Multiple concurrent `database_t` instances at different paths are supported.
  `database_config_set_sync_only(cfg, 1)` disables MVCC for single-threaded
  use (appropriate for a vector index).

**This is the fact the original SK-LSH proposal got wrong.** It assumed a
backward cursor / bi-directional scanning existed. SK-LSH's "scan outward both
directions from the query bucket" is NOT natively possible. Every index type
here is designed so neighbor retrieval is expressible as forward range scans
only.

## The two index types + how each maps to forward scans

Keys use delimiter `/`; a prefix scan is `start = prefix`, `end = prefix +
"\x7f"` (the DEL char as an "everything under prefix" upper bound).

### A. IVF (Inverted File) — the native fit

IVF needs NO backward scan. Every operation is a forward prefix scan.

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

**Insert (atomic batch)** — one `database_batch_sync_raw`:
- put `vec/{index}/vector/{id}` -> raw vector
- put `vec/{index}/cluster/{cid}/{id}` -> id (membership)
- increment `vec/{index}/count`

**Training** — k-means: assign each vector to nearest centroid, recompute
centroids, repeat. Periodic rebuild via `vector_layer_train`. Per-insert
assigns to the current nearest centroid; centroids are refreshed at train
time, not per insert.

**Cold-start hybrid (flat-until-N)** — with `< N` vectors k-means is
degenerate (empty/singleton clusters). Below `N` (e.g. `n_clusters * 10`,
default ~1000), search falls back to **exact flat**: prefix-scan
`vec/{index}/vector/`, brute-force distance, top-k. Above `N`, switch to
IVF. This avoids the degenerate-cluster failure mode and gives an exact
baseline for free.

### B. SK-LSH (SortingKeys-LSH) — dual-index workaround

SK-LSH hashes vectors to **sortable compound LSH keys** so that keys close in
sort order correspond to vectors close in distance. Stored sorted, query =
seek to position + bi-directional expansion. The bi-directional part is the
problem; the dual-index is the solution.

**Key layout**
- `vec/{index}/vector/{id}` -> raw vector.
- `vec/{index}/hash/{lsh_key}/{id}` -> id (forward-ordered index).
- `vec/{index}/ihash/{inv_lsh_key}/{id}` -> id (bit-inverted index; "left
  neighbors" of the original become "right neighbors" here).
- `vec/{index}/proj/{t}` -> projection matrix for LSH table `t`.

**The left-neighbor problem + solution.** Hash the query -> `q_key`.
"Right" neighbors (`lsh_key > q_key`): forward scan from
`start=vec/{index}/hash/{q_key}/`, depth `scan_radius`. Easy. "Left" neighbors
(`lsh_key < q_key`): there is no backward scan, and restarting from the start
of the hash range scans everything to the left of `q_key` (unviable at scale).
The dual-index writes a second copy under the **bit-inverted** key
`{~lsh_key}`; left neighbors of the original are right neighbors in the
inverted index. So bi-directional expansion = TWO forward scans: one over
`hash/`, one over `ihash/`.

**Query flow**
1. Hash query -> `q_key`; compute `~q_key`.
2. Forward scan `hash/` from `{q_key}/`, depth `scan_radius` -> right
   candidates.
3. Forward scan `ihash/` from `{~q_key}/`, depth `scan_radius` -> left
   candidates (in original space).
4. Dedup, fetch vectors, **exact rerank**, top-k.

**Insert (atomic batch)** — one `database_batch_sync_raw`:
- put `vec/{index}/vector/{id}` -> raw vector
- put `vec/{index}/hash/{lsh_key}/{id}` -> id
- put `vec/{index}/ihash/{inv_lsh_key}/{id}` -> id (dual index)

**Cost:** 2x index storage + 3 writes per insert (vector + 2 index entries)
vs IVF's 2 writes (vector + 1 membership). No training (random projections
only), simpler hash. **Recall ~90-95%, lossy** (bucket boundaries, curse of
dimensionality, capped `scan_radius`; rerank fixes false positives, not false
negatives). A `slsh_dual_index` config toggle allows degrading to right-only
(1 scan, 2 writes) for write-light/read-cheap cases at a recall cost — the
spike measures whether dual is worth it.

### Comparison

| | IVF | SK-LSH |
|---|---|---|
| Fit for forward-only scans | native (prefix scans) | needs dual-index workaround |
| Backward scan needed? | no | no (worked around) |
| Recall | ~90-95% (tunable nprobe) | ~90-95% (tunable scan_radius) |
| Storage vs flat | ~1x + small centroid set | ~2x (dual index) |
| Writes per insert | 2 (+ count) | 3 |
| Training | k-means (periodic) | none (random projections) |
| Cold-start | degenerate -> needs flat-until-N | fine from 1 vector |
| Exact baseline | flat-until-N gives it free | flat fallback adds it |

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
    VL_INDEX_SLSH = 2    /* SK-LSH dual-index */
} vl_index_type_t;

typedef struct {
    vl_index_type_t index_type;
    int      dim;                 /* vector dimensionality */
    char     delimiter;           /* path-segment separator, default '/' */
    int      top_k;               /* default result count */
    int      sync_only;           /* 1 = single-threaded, disable MVCC */
    /* IVF */
    int      ivf_n_clusters;
    int      ivf_nprobe;
    int      ivf_flat_until;      /* exact flat below this many vectors */
    /* SK-LSH */
    int      slsh_lsh_tables;     /* compound hash width (L) */
    int      slsh_hash_bits;      /* bits per table */
    float    slsh_bucket_width;   /* LSH projection width W */
    int      slsh_scan_radius;    /* forward scan depth each direction */
    int      slsh_dual_index;     /* 1 = dual (inverted) index */
} vector_layer_config_t;

typedef struct {
    char    *id;
    float    distance;
    uint8_t *metadata;            /* caller frees via vector_layer_free_results */
    size_t   metadata_len;
} vl_result_t;

/* Open on an EXISTING database_t (shared key space, e.g. with Graph). */
vector_layer_t* vector_layer_create(const char *index_name,
                                    database_t *db,
                                    vector_layer_config_t *config,
                                    int *error_code);

/* Open a DEDICATED vector_db instance at db_location (clean LRU/WAL tuning,
   mirrors the Hippo document_db split). */
vector_layer_t* vector_layer_open_separate(const char *db_location,
                                           const char *index_name,
                                           vector_layer_config_t *config,
                                           int *error_code);

void  vector_layer_destroy(vector_layer_t *vl);

/* Insert is atomic: vector + index entries in one database_batch_sync_raw. */
int   vector_layer_insert(vector_layer_t *vl, const char *id,
                          const float *vec,
                          const uint8_t *metadata, size_t metadata_len);
int   vector_layer_insert_batch(vector_layer_t *vl,
                                const char **ids, const float **vecs,
                                const uint8_t **metadatas, const size_t *meta_lens,
                                int n);

int   vector_layer_search(vector_layer_t *vl, const float *query, int k,
                          vl_result_t **results, int *n_results);
void  vector_layer_free_results(vl_result_t *results, int n);

int   vector_layer_delete(vector_layer_t *vl, const char *id);
int   vector_layer_train(vector_layer_t *vl);   /* IVF: (re)compute centroids.
                                                    SLSH: (re)gen projections */
int   vector_layer_rebuild(vector_layer_t *vl);
size_t vector_layer_count(vector_layer_t *vl);

#ifdef __cplusplus
}
#endif
#endif
```

Every public function translates to `database_*_sync_raw` +
`database_batch_sync_raw` against the layer's `database_t*`. The layer never
touches `hbtrie_cursor_*` (un-exported). The exact-flat path
(`VL_INDEX_FLAT` / IVF below `flat_until`) is a single
`database_scan_range_sync_raw` over `vec/{index}/vector/` + in-C brute force.

## Build + bindings wiring

Following the Graph layer precedent end-to-end:

**C / CMake** (root `CMakeLists.txt`):
1. Create `src/Layers/vector/vector_layer.c` + `vector_layer.h` (`extern
   "C"` guards, opaque struct, the API above).
2. Add `src/Layers/vector/vector_layer.c` to `WAVEDB_SOURCES` (after the
   Graph block, ~`:131`). This auto-builds it into BOTH `wavedb` (static) and
   `wavedb_shared` (FFI shared -> `libwavedb.so/.dll/.dylib`).
3. Add every exported `vector_layer_*` symbol to `src/wavedb.def` (MSVC DLL
   export — this IS the FFI surface).

**C test** (`tests/test_vector.cpp`, GoogleTest):
- `add_executable(test_vector tests/test_vector.cpp)`;
  `target_link_libraries(test_vector wavedb gtest gtest_main)`;
  `add_test(NAME test_vector COMMAND test_vector)` — mirroring the Graph
  test block (`CMakeLists.txt:384-386`). Uses `extern "C"` include, temp-dir
  fixture, `ASSERT_NE(nullptr, ...)`.

**Python bindings** (cffi, out-of-line + ABI fallback):
4. Add `vector_layer_*` cdef entries to BOTH `_cffi_build.py` (compiled) and
   `_native_abi.py` (ABI fallback) — they must stay in sync (the codebase
   duplicates them verbatim). Add `typedef struct vector_layer_t
   vector_layer_t;` to the types cdef block in both, and to the `_C_SOURCE`
   forward-declaration preamble in `_cffi_build.py`.
5. Add `src/wavedb/vector_layer.py` wrapper (mirror `graph_layer.py`: hold
   the opaque `ffi` pointer, call `lib.vector_layer_*`, encode strings, check
   rc, `map_error`, wrap lifecycle in `__enter__/__exit__/__del__`).
6. Export `VectorLayer` from `src/wavedb/__init__.py`.
7. `tests/test_vector.py` (pytest, reuse the `db_path` fixture from
   `conftest.py`).
8. Re-run `bindings/python/scripts/copy_sources.py` before building an sdist
   so `c_src/` reflects the new `src/Layers/vector/` files.

**Dart** (`bindings/dart/`): add `lookupFunction<...C, ...>('symbol')`
entries to `wavedb_bindings.dart` + a `vector_layer.dart` wrapper, mirroring
`graph_layer.dart`. Consumes the same `wavedb_shared` target.

**Node** (`bindings/nodejs/`): optional in the spike (the spike can ship C +
Python only); a `vector_layer.cc` N-API wrapper follows later if Node is
needed.

No `setup.py` / `pyproject.toml` changes are required — the build pipeline
is generic (it globs the C tree and the cdef list).

## Spike test

A **throwaway** experiment to decide which index (or hybrid) to build fully.
Its job is to pick the index, not to be production. Minimal C for each path,
no delete/train-rebuild polish.

**Objective / decision gate.** On a toy corpus, compare IVF (flat-until-N +
IVF), SK-LSH (dual-index, and right-only), and exact-flat (baseline) on:
- **recall@10** vs exact ground truth,
- **p50 / p99 search latency**,
- **insert throughput** (ops/sec),
- **storage size** (index bytes / vector).

Decision: pick the index that meets `recall >= 0.90@10` at acceptable latency
and storage for the target scale. If IVF and SK-LSH are close, prefer IVF
(native fit, no 2x storage, no backward-scan workaround). If both fail the
gate, ship exact-flat behind the API and revisit. If SK-LSH dual vs
right-only are within recall noise, prefer right-only (1 scan, 2 writes).

**Corpus.** 10k-50k vectors; **384-dim** (bge-small-en-v1.5) as primary,
plus a 768/1536-dim variant for scaling. Synthetic (random gaussian,
clustered blobs) so ground-truth NN is known exactly by brute force. If a
real embedding subset is cheap to produce, add it as a second arm.

**Minimal implementation.**
- C: `vector_layer_create`/`destroy`/`insert`/`search` for FLAT, IVF, SLSH.
  IVF: random-init centroids or one-shot k-means; SLSH: random projections
  (fixed seed). Exact rerank for all three.
- Python: the `vector_layer.py` wrapper so a benchmark script can drive it.
- A `bench/` harness (pytest or a small C bench linked to `wavedb`) that
  loads the corpus, builds ground truth by exact flat, runs the search arms,
  prints recall/latency/throughput/storage.

**Throwaway discipline.** The spike code is a branch / scratch; only the
**decision + the winning index's C** carries forward. The losing index's C
is deleted (or kept behind the `vl_index_type_t` enum if trivial). No
production hardening (no delete, no incremental rebuild, no concurrent
writers) until the spike decides.

## Test plan (beyond the spike)

- `tests/test_vector.cpp` (gtest): create (shared db + open_separate),
  insert N vectors, search returns top-k with correct ids, exact-flat
  matches brute force, IVF flat-until-N switch at the threshold, SLSH
  dual-index returns both-neighbor candidates, delete removes from all
  index subtrees, count is correct, atomic-batch rollback on a forced
  mid-batch failure leaves no partial index entries.
- `bindings/python/tests/test_vector.py` (pytest): same surface via the
  `VectorLayer` wrapper; round-trip metadata; error mapping via
  `map_error`.
- Regression: the existing Graph/GraphQL/subtree suites stay green (new
  layer is purely additive; no shared symbols touched).

## Honest caveats / risks

- **R1 backward-scan:** the original SK-LSH proposal assumed a backward
  cursor that does not exist. SK-LSH's cost (2x storage / 3 writes, or
  right-only recall loss) is the direct consequence. The spike must justify
  it vs IVF.
- **R2 IVF cold-start:** degenerate clusters below the flat-until-N
  threshold; the hybrid is mandatory, not optional. Threshold is a knob.
- **R3 recall is approximate:** both IVF and SK-LSH lose recall; exact-flat
  is the baseline AND the cold-start fallback. Never claim exact recall for
  the ANN paths.
- **R4 embedding quality (out of scope here):** any downstream win that
  consumes the layer depends on embeddings capturing relational/functional
  similarity, not surface co-occurrence. That is the consumer's concern, not
  the VectorLayer's — the layer is similarity + storage, nothing more.
- **R5 atomicity unit = one batch.** No explicit begin/commit/rollback; a
  failed `database_batch_sync_raw` leaves no partial writes (WAL_BATCH is
  atomic), but the layer must not issue index puts outside a batch.
- **R6 subtrees vs separate db:** a separate `vector_db` (clean LRU/WAL) is
  recommended over a subtree when the host DB has a hot LRU to protect.
  `vector_layer_open_separate` is the default path there.
- **R7 MSVC export discipline:** forgetting a symbol in `wavedb.def` means it
  resolves at build on GCC/Clang but fails on MSVC — the inverse of the
  `stddef.h` lesson. Every `vector_layer_*` goes in `wavedb.def` from day
  one.
- **R8 cdef sync:** `_cffi_build.py` and `_native_abi.py` are
  hand-duplicated; drifting them silently breaks the ABI fallback. Treat
  them as one list.

## Open questions for the spike to resolve

- Does IVF meet `recall >= 0.90@10` at `nprobe` <= 8 on 50k x 384, and is the
  centroid-scan cost acceptable as cluster count grows?
- Is SK-LSH dual-index's 2x storage worth it over right-only, or does
  right-only already clear the recall gate?
- What `flat_until` threshold keeps cold-start exact without dominating
  memory at scale?
- 384-dim vs 768/1536-dim: does recall degrade enough at higher dim (curse
  of dimensionality) to force a dimensionality-reduction step (PCA/Hilbert)
  before hashing — a third index arm the spike should leave room for?

## Status / next

This is the design. After the spike, an executable checkbox plan lands at
`docs/superpowers/plans/2026-07-NN-vector-layer.md` (Phase/Task/Step template)
for the winning index's full build, and the losing index is dropped.
De-wonk runs at the completion gate of that implementation, not now.