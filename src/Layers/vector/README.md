# WaveDB Vector Layer

ANN vector similarity search over WaveDB's sorted HBTrie. Pure C, consumed
by the Python/Dart/Node bindings. Three index types:

- **FLAT** — exact brute-force. Baseline + IVF cold-start fallback. recall@10
  ~= 1.0 by definition.
- **IVF** — inverted file (k-means clustering + per-cluster forward scan).
  Default for clustered embedding workloads.
- **SLSH** — sortable compound LSH (SK-LSH style) with bidirectional
  forward+backward scan. Handles uniform distributions well.

Config is split into a **format tier** (immutable after create — drop the
subtree / separate db and recreate to change) and a **runtime tier** (freely
mutable via `vector_layer_reconfigure`). The C signature of
`vector_layer_reconfigure` accepts only the runtime tier; format mutation is
structurally impossible without drop+recreate.

See `docs/superpowers/specs/2026-07-12-vector-layer-design.md` for the full
design spec and `bench/vector/REPORT.md` for the spike data backing the
effect columns below.

## Config reference

### Format tier (immutable after create)

| Field | Type | Default | Effect on recall | Effect on latency | Effect on storage | When to tune |
|---|---|---|---|---|---|---|
| `index_type` | enum | FLAT | FLAT = 1.0 (exact); IVF ~0.96-0.99 on clustered, ~0.36-0.48 on gaussian; SLSH ~0.91-0.95 on clustered (10k), drops on uniform | FLAT O(N); IVF O(nprobe·N/K); SLSH O(radius) | FLAT 1x; IVF +~86 bytes/vec over FLAT (centroids + membership); SLSH +~89 bytes/vec (hash entries) | Choose per workload: FLAT for small/exact, IVF for clustered, SLSH for uniform |
| `dim` | int | — (required) | Higher dim → curse of dimensionality → lower ANN recall (gaussian especially) | Linear in dim (FLAT p50 64ms@384 → 162ms@768 on 10k) | Linear in dim (1687 → 3231 bytes/vec FLAT, 1773 → 3325 IVF, 1776 → 3321 SLSH) | Match your embedding model exactly |
| `delimiter` | char | '/' | — (negligible) | — (negligible) | — (negligible) | Rarely; only if '/' conflicts with your id scheme |
| `distance` | enum | COSINE | — (used for assignment + rerank; choice does not change recall at fixed index) | — (negligible — same float ops) | — (negligible) | Match your embedding model's metric (COSINE for normalized embeddings, L2 for general, DOT for inner-product) |
| `ivf_n_clusters` | int | 50 | More clusters → finer granularity → higher recall (diminishing); 50 clears 0.90 on 10k & 30k clustered | More centroids to scan at query (small); training is O(K²·N·dim) for k-means++ | Small (K·dim floats + membership keys) | ~sqrt(N) is a rule of thumb; 50 for 10k, 170 for 30k. Raise if recall plateaus |
| `slsh_lsh_tables` | int | 4 | More tables → finer hash → higher recall (diminishing; spike used 4) | More hash entries per insert (linear in L) | Linear in L | 2-4 for clustered; 4-8 for uniform |
| `slsh_hash_bits` | int | 16 | More bits → finer buckets → higher recall up to sparsity | — (small) | — (small) | 8-16; 16 is standard. Lower only if buckets get too sparse |
| `slsh_bucket_width` | float | 2.0 | Smaller W → finer buckets → higher recall up to sparsity; spike saw 0.945 at W=2.0, 0.931 at W=10.0 (10k×384) | W=2.0 → 4.9ms p50; W=10.0 → 45ms p50 (more buckets to scan at wide W) | — (negligible) | 1.0-4.0; **2.0 default gives best latency at equal recall**; tune on your data |

### Runtime tier (mutable via `vector_layer_reconfigure`)

| Field | Type | Default | Effect on recall | Effect on latency | Effect on storage | When to tune |
|---|---|---|---|---|---|---|
| `top_k` | int | 10 | — (recall@k metric is k) | Linear in k (rerank cost) | — (negligible) | Match your use case |
| `sync_only` | int | 1 | — (no effect on results) | sync_only=1: single-threaded, no MVCC overhead, faster for single-caller workloads; 0: enables async worker pool, parallel inserts/searches | — (negligible) | 1 for single-threaded / simple scripts; 0 for concurrent multi-caller workloads |
| `ivf_nprobe` | int | 8 | Higher → more clusters probed → higher recall; spike: 8 → 0.96-0.99 (clustered); 16 → 0.60-0.77 (gaussian) | Linear in nprobe; spike: 8 → 22-76ms p50; 16 → 81-202ms p50 | — (negligible) | 8 default clears 0.90 on clustered; raise to 16-32 if your data is near-uniform; cost is linear |
| `ivf_flat_until` | int | 1000 | Below this count, FLAT (exact) is used → recall 1.0; above, IVF runs → recall 0.96-0.99 | FLAT is O(N) but N is small here; no degradation observed | — (negligible) | 1000 default; raise if cold-start recall is low on your data; lower only if you want IVF from insert 1 |
| `slsh_scan_radius` | int | 200 | Higher → more candidates → higher recall; spike: 200 → 0.945 (10k×384), 0.908 (10k×768); 500 → 0.976 (10k×768); 1000 → 0.975 (30k×384) | Linear in radius; spike: 200 → 5ms p50, 500 → 19ms p50, 1000 → 45ms p50 (10k×384) | — (negligible) | **ADAPTIVE: actual = max(configured, count/30).** Configured value is a floor; scales automatically with dataset size (~333 for 10k, ~1000 for 30k, ~1666 for 50k). Raise the floor for more recall on small datasets; let auto-scaling handle large datasets |

## Index choice guide

| Workload | Recommended index | Why |
|---|---|---|
| Small N (< `ivf_flat_until`, default 1000) | FLAT | Exact, no training, no tuning; IVF falls back to FLAT here anyway |
| Large N, clustered embeddings | IVF | Native fit for forward scans; k-means++ exploits cluster structure; clears 0.90 at nprobe=8 |
| Large N, uniform / gaussian embeddings | SLSH or FLAT | LSH handles uniform distributions better than k-means; FLAT if you need exact |
| Need exact results | FLAT | recall@10 = 1.0 by definition (0.976-0.987 on clustered is tie-breaking at equal distances) |
| Write-heavy, read-light | IVF | ~2 writes/insert (vector + cluster membership); train is periodic |
| Cold-start (growing dataset) | IVF with `ivf_flat_until` | FLAT fallback avoids degenerate k-means at low N |
| Single-threaded simple workload | any, `sync_only=1` | No MVCC overhead |
| Concurrent multi-caller workload | any, `sync_only=0` | Async worker pool parallelizes inserts/searches |

## Performance characteristics (from the spike)

10k/30k × 384/768-dim synthetic corpora; 100 queries per corpus; clustered
= 50 cluster centers with tight blobs, gaussian = N(0,1) (worst case for
ANN). Full numbers in `bench/vector/REPORT.md`.

### FLAT (baseline, exact)

| Corpus | recall@10 | p50 (ms) | p99 (ms) | insert ops/s | storage/vec (bytes) |
|---|---|---|---|---|---|
| syn_10000_384 | 1.000 | 64 | 86 | 581 | 1687 |
| clu_10000_384 | 0.986 | 60 | 101 | 528 | 1687 |
| clu_30000_384 | 0.987 | 201 | 377 | 499 | 1687 |
| syn_10000_768 | 1.000 | 162 | 544 | 461 | 3231 |
| clu_10000_768 | 0.976 | 78 | 89 | 599 | 3231 |

### IVF (n_clusters=50, flat_until=500)

| Corpus | nprobe | recall@10 | p50 (ms) | p99 (ms) | insert ops/s | storage/vec (bytes) | Gate |
|---|---|---|---|---|---|---|---|
| clu_10000_384 | 8 | 0.960 | 22 | 99 | 1076 | 1773 | PASS (≥0.90) |
| clu_30000_384 | 8 | 0.987 | 76 | 306 | 835 | 1772 | PASS |
| clu_10000_768 | 8 | 0.967 | 34 | 167 | 726 | 3325 | PASS |
| syn_10000_384 | 8 | 0.363 | 69 | 474 | 1072 | 1773 | n/a (gaussian) |
| syn_10000_768 | 16 | 0.770 | 202 | 370 | 633 | 3325 | n/a (gaussian) |

IVF is 3-10x faster than FLAT at p50 (22ms vs 60ms @ 10k, 76ms vs 201ms @
30k) with 0.96-0.99 recall on clustered data. Gaussian is the expected worst
case (~0.36-0.48) — neighbors spread across clusters.

### SLSH (lsh_tables=4, hash_bits=16, bucket_width=2.0, bidirectional)

| Corpus | scan_radius | recall@10 | p50 (ms) | p99 (ms) | insert ops/s | storage/vec (bytes) | Gate |
|---|---|---|---|---|---|---|---|
| clu_10000_384 | 200 | 0.945 | 4.9 | 155 | 1108 | 1776 | PASS (≥0.90) |
| clu_10000_768 | 200 | 0.908 | 10 | 256 | 785 | 3321 | PASS |
| clu_10000_768 | 500 | 0.976 | 19 | 155 | 940 | 3321 | PASS |
| clu_30000_384 | 200 | 0.557 | 5.8 | 454 | 1027 | 1780 | FAIL (fixed by adaptive floor) |
| clu_30000_384 | 1000 | 0.975 | 45 | 519 | 869 | 1780 | PASS (auto-scaled) |

SLSH is 5-12x faster than FLAT at p50 (5ms vs 60ms @ 10k). At 30k, radius=200
covers only ~1.3% of the dataset and fails; the adaptive floor
(`max(200, count/30)` ≈ 1000 at 30k) auto-scales to PASS without
reconfiguration. `bucket_width=2.0` gives 9x lower latency than `10.0` at
equal recall (4.9ms vs 45ms p50).

### Storage overhead

IVF and SLSH add ~86-94 bytes/vector over FLAT (1687 → 1773 IVF, 1687 → 1776
SLSH at 384-dim; 3231 → 3325 IVF, 3231 → 3321 SLSH at 768-dim) for cluster
membership / hash keys. The vector payload itself dominates (4·dim bytes).

## API

The vector layer is built on top of an existing `database_t` (shared key
space, optionally scoped to a `database_subtree_t`) or a dedicated
per-instance database opened by `vector_layer_open_separate`. Construction,
sync, async, train, rebuild, and reconfigure entry points are declared in
`vector_layer.h`. Async variants take a caller-owned `promise_t*` and
resolve with `NULL` (insert/delete/batch) or a `vl_search_result_t*`
(search); on `sync_only=1` or no worker pool, async runs inline and the
promise resolves before the call returns. Full signature reference and the
async error contract are in the spec:
`docs/superpowers/specs/2026-07-12-vector-layer-design.md`.

## Build

The vector layer is compiled into `libwavedb` automatically by the top-level
CMake; no special build flag is required. Source files live in
`src/Layers/vector/` (`vector_layer.c` + `vector_flat.c` / `vector_ivf.c` /
`vector_slsh.c` + `vector_distance.c` + `vector_internal.c`). Bindings pick
the layer up via the existing `libwavedb` link.