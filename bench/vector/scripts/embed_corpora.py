#!/usr/bin/env python3
"""Generate vector corpora for the WaveDB vector layer spike.

Outputs bench/vector/corpus/{name}.fvec (raw float[dim] row-major,
header {dim, count, query_count} as uint32 LE) + {name}.gt (top-10 ids
per query, int32 LE, shape [query_count, 10]).

Synthetic arms: gaussian + clustered_blobs at 10k/30k/50k x 384/768/1536-dim.
Real arm (optional): 10-30k EnterpriseRAG-Bench docs embedded with
bge-small-en-v1.5 via sentence-transformers.

If the real arm is infeasible (no internet, sentence-transformers not
installed, model not downloadable, or source dir lacks docs), it falls
back to a synthetic clustered corpus. The spec allows this: "If the real
arm fails to generate, the spike can proceed with synthetic only."

Usage:
  python3 embed_corpora.py --synthetic
  python3 embed_corpora.py --real --source <path-to-EnterpriseRAG-Bench-source> --real-count 30000
  python3 embed_corpora.py --synthetic --counts 1000 --dims 16   # smoke test
"""
import numpy as np
import os
import struct
import argparse
import sys
import json

CORPUS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "corpus")
K_GT = 10  # top-10 ground truth
N_QUERIES = 100


def write_fvec(path, dim, vectors, queries):
    """Write .fvec: uint32 LE {dim, count, query_count} header
    + count*dim float32 (vectors, row-major) + query_count*dim float32 (queries)."""
    vectors = np.ascontiguousarray(np.asarray(vectors, dtype=np.float32))
    queries = np.ascontiguousarray(np.asarray(queries, dtype=np.float32))
    with open(path, "wb") as f:
        f.write(struct.pack("<III", dim, vectors.shape[0], queries.shape[0]))
        f.write(vectors.tobytes())
        f.write(queries.tobytes())


def write_gt(path, vectors, queries, k=K_GT):
    """Brute-force top-k nearest neighbors (L2) per query.

    Uses the ||v-q||^2 = ||v||^2 - 2 v.q + ||q||^2 identity to avoid
    materializing the (count, dim) difference array per query. For a
    50k x 1536 corpus, that saves ~300MB of transient allocation per
    query and is also noticeably faster.
    """
    vectors = np.ascontiguousarray(np.asarray(vectors, dtype=np.float32))
    queries = np.ascontiguousarray(np.asarray(queries, dtype=np.float32))
    sq_norms = (vectors * vectors).sum(axis=1)  # (count,)
    gt = np.zeros((queries.shape[0], k), dtype=np.int32)
    for i in range(queries.shape[0]):
        q = queries[i]
        # ||v - q||^2 = sq_norms - 2 v.q + ||q||^2; ||q||^2 is constant across v.
        dists = sq_norms - 2.0 * (vectors @ q)
        # argpartition gives the k smallest (unordered); sort by distance for stability.
        part = np.argpartition(dists, k)[:k]
        order = part[np.argsort(dists[part])]
        gt[i] = order.astype(np.int32)
    gt.tofile(path)


def gen_synthetic(name, count, dim, seed=42, clustered=False):
    rng = np.random.default_rng(seed)
    if clustered:
        # 50 cluster centers, well-separated (scale 10), tight blobs (sigma 0.1).
        centers = rng.normal(0, 10, size=(50, dim)).astype(np.float32)
        assignments = rng.integers(0, 50, size=count)
        vectors = (centers[assignments] + rng.normal(0, 0.1, size=(count, dim))).astype(np.float32)
        # Realistic queries: near cluster centers (center + noise). This matches
        # the gtest's vl_recall_against_stored_clustered query distribution —
        # gaussian queries far from all centers are an unrealistic worst case
        # that no ANN index can serve well.
        query_assignments = rng.integers(0, 50, size=N_QUERIES)
        queries = (centers[query_assignments] + rng.normal(0, 0.1, size=(N_QUERIES, dim))).astype(np.float32)
    else:
        vectors = rng.normal(0, 1, size=(count, dim)).astype(np.float32)
        queries = rng.normal(0, 1, size=(N_QUERIES, dim)).astype(np.float32)
    os.makedirs(CORPUS_DIR, exist_ok=True)
    fvec_path = os.path.join(CORPUS_DIR, f"{name}.fvec")
    gt_path = os.path.join(CORPUS_DIR, f"{name}.gt")
    write_fvec(fvec_path, dim, vectors, queries)
    write_gt(gt_path, vectors, queries)
    print(f"  wrote {name}.fvec ({count}x{dim}) + {name}.gt ({N_QUERIES} queries)")


def _load_jsonl_docs(path, limit):
    docs = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except Exception:
                continue
            # EnterpriseRAG-Bench JSONL fields: adapt to actual schema.
            # Try common field names; fall through to first string-valued field.
            text = (obj.get("text") or obj.get("content") or obj.get("body")
                    or obj.get("page_content") or obj.get("document") or "")
            if not text and isinstance(obj, dict):
                for v in obj.values():
                    if isinstance(v, str) and len(v) > 20:
                        text = v
                        break
            if text:
                docs.append(text)
            if len(docs) >= limit:
                break
    return docs


def gen_real(name, source_dir, count, dim=384):
    """Embed `count` docs from source_dir using bge-small-en-v1.5.

    Falls back to synthetic clustered if sentence-transformers, the model,
    or sufficient docs are unavailable.
    """
    try:
        from sentence_transformers import SentenceTransformer
    except ImportError:
        print(f"  [fallback] sentence-transformers not installed; "
              f"generating synthetic {name} instead", file=sys.stderr)
        gen_synthetic(name, count, dim, clustered=True)
        return
    try:
        model = SentenceTransformer("BAAI/bge-small-en-v1.5")
    except Exception as e:
        print(f"  [fallback] could not load bge-small-en-v1.5: {e}; "
              f"generating synthetic {name} instead", file=sys.stderr)
        gen_synthetic(name, count, dim, clustered=True)
        return

    # Load docs from source_dir. EnterpriseRAG-Bench distributes docs as
    # JSONL files (possibly within zip archives). Walk recursively and
    # read .jsonl files. Adapt to whatever's in source_dir.
    docs = []
    for root, _dirs, files in os.walk(source_dir):
        for fname in sorted(files):
            if fname.endswith(".jsonl"):
                docs.extend(_load_jsonl_docs(os.path.join(root, fname), count - len(docs)))
            if len(docs) >= count:
                break
        if len(docs) >= count:
            break

    if len(docs) < count:
        print(f"  [fallback] only {len(docs)} docs found in {source_dir} "
              f"(need {count}); generating synthetic {name} instead", file=sys.stderr)
        gen_synthetic(name, count, dim, clustered=True)
        return

    docs = docs[:count]
    print(f"  embedding {len(docs)} docs with bge-small-en-v1.5...")
    vectors = model.encode(docs, show_progress_bar=True).astype(np.float32)

    # 500 query vectors: sample from the same doc set. Use the last 500 docs
    # (disjoint from the first count-500 when count > 500) as queries.
    if count > N_QUERIES:
        query_docs = docs[count - N_QUERIES:]
    else:
        query_docs = docs[:]
    queries = model.encode(query_docs[:N_QUERIES], show_progress_bar=True).astype(np.float32)

    os.makedirs(CORPUS_DIR, exist_ok=True)
    fvec_path = os.path.join(CORPUS_DIR, f"{name}.fvec")
    gt_path = os.path.join(CORPUS_DIR, f"{name}.gt")
    write_fvec(fvec_path, vectors.shape[1], vectors, queries)
    write_gt(gt_path, vectors, queries)
    print(f"  wrote {name}.fvec ({len(vectors)}x{vectors.shape[1]}) + "
          f"{name}.gt ({queries.shape[0]} queries)")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--synthetic", action="store_true",
                        help="generate synthetic corpora")
    parser.add_argument("--real", action="store_true",
                        help="generate real EnterpriseRAG-Bench corpus")
    parser.add_argument("--source", default=None,
                        help="path to EnterpriseRAG-Bench source dir (for --real)")
    parser.add_argument("--real-count", type=int, default=30000,
                        help="number of real docs to embed (default 30000)")
    parser.add_argument("--dims", nargs="*", type=int, default=[384, 768, 1536],
                        help="synthetic dims (default 384 768 1536)")
    parser.add_argument("--counts", nargs="*", type=int, default=[10000, 30000, 50000],
                        help="synthetic counts (default 10000 30000 50000)")
    args = parser.parse_args()

    if args.synthetic:
        print("Generating synthetic corpora...")
        for count in args.counts:
            for dim in args.dims:
                gen_synthetic(f"syn_{count}_{dim}", count, dim, clustered=False)
                gen_synthetic(f"clu_{count}_{dim}", count, dim, clustered=True)
    if args.real:
        if args.source is None:
            print("--real requires --source <path>", file=sys.stderr)
            sys.exit(1)
        gen_real("real_subset", args.source, args.real_count, dim=384)
    if not args.synthetic and not args.real:
        parser.print_help()


if __name__ == "__main__":
    main()