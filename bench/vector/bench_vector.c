/* C bench driver for the vector layer spike.
   Usage: ./bench_vector <corpus> <index_type:flat|ivf|slsh> <k> [nprobe] [scan_radius] [flat_until] [bidirectional]
   Outputs JSON: recall@10, p50, p99, insert_throughput, storage_per_vector.

   Reads bench/vector/corpus/{corpus}.fvec + {corpus}.gt. */
#include "../../src/Layers/vector/vector_layer.h"
#include "../../src/Database/database.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Sum all file sizes in a directory (recursively). */
static long dir_size(const char *path) {
    DIR *d = opendir(path);
    if (!d) return 0;
    long total = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char child[1024];
        snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(child, &st) == 0) {
            if (S_ISDIR(st.st_mode)) total += dir_size(child);
            else total += st.st_size;
        }
    }
    closedir(d);
    return total;
}

static int dbl_cmp(const void *a, const void *b) {
    double da = *(const double*)a, db = *(const double*)b;
    return (da > db) - (da < db);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <corpus> <index_type:flat|ivf|slsh> <k> [nprobe] [scan_radius] [flat_until] [bidirectional]\n", argv[0]);
        return 1;
    }
    const char *corpus = argv[1];
    const char *itype = argv[2];
    int k = atoi(argv[3]);

    /* Load corpus. */
    char fvec_path[256];
    snprintf(fvec_path, sizeof(fvec_path), "bench/vector/corpus/%s.fvec", corpus);
    FILE *f = fopen(fvec_path, "rb");
    if (!f) { perror("fopen fvec"); return 1; }
    uint32_t dim, count, qcount;
    if (fread(&dim, 4, 1, f) != 1 || fread(&count, 4, 1, f) != 1 || fread(&qcount, 4, 1, f) != 1) {
        fprintf(stderr, "failed to read fvec header\n"); fclose(f); return 1;
    }
    float *vectors = malloc((size_t)count * dim * 4);
    float *queries = malloc((size_t)qcount * dim * 4);
    if (!vectors || !queries) { fprintf(stderr, "OOM loading corpus\n"); fclose(f); return 1; }
    if (fread(vectors, 4, (size_t)count * dim, f) != (size_t)count * dim) {
        fprintf(stderr, "failed to read vectors\n"); fclose(f); free(vectors); free(queries); return 1;
    }
    if (fread(queries, 4, (size_t)qcount * dim, f) != (size_t)qcount * dim) {
        fprintf(stderr, "failed to read queries\n"); fclose(f); free(vectors); free(queries); return 1;
    }
    fclose(f);

    /* Load ground truth. */
    char gt_path[256];
    snprintf(gt_path, sizeof(gt_path), "bench/vector/corpus/%s.gt", corpus);
    f = fopen(gt_path, "rb");
    if (!f) { perror("fopen gt"); free(vectors); free(queries); return 1; }
    int32_t *gt = malloc((size_t)qcount * 10 * 4);
    if (!gt) { fprintf(stderr, "OOM gt\n"); fclose(f); free(vectors); free(queries); return 1; }
    if (fread(gt, 4, (size_t)qcount * 10, f) != (size_t)qcount * 10) {
        fprintf(stderr, "failed to read gt\n"); fclose(f); free(vectors); free(queries); free(gt); return 1;
    }
    fclose(f);

    /* Build config. */
    vector_layer_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.format.dim = (int)dim;
    cfg.format.delimiter = '/';
    cfg.format.distance = VL_DIST_L2;
    if (strcmp(itype, "flat") == 0) {
        cfg.format.index_type = VL_INDEX_FLAT;
    } else if (strcmp(itype, "ivf") == 0) {
        cfg.format.index_type = VL_INDEX_IVF;
        cfg.format.ivf_n_clusters = 50;
        cfg.runtime.ivf_nprobe = argc > 4 ? atoi(argv[4]) : 8;
        cfg.runtime.ivf_flat_until = argc > 6 ? atoi(argv[6]) : 500;
    } else if (strcmp(itype, "slsh") == 0) {
        cfg.format.index_type = VL_INDEX_SLSH;
        cfg.format.slsh_lsh_tables = 4;
        cfg.format.slsh_hash_bits = 16;
        cfg.format.slsh_bucket_width = 2.0f;
        cfg.runtime.slsh_scan_radius = argc > 5 ? atoi(argv[5]) : 50;
        cfg.runtime.slsh_bidirectional = argc > 7 ? atoi(argv[7]) : 1;
    } else {
        fprintf(stderr, "unknown index_type: %s\n", itype);
        free(vectors); free(queries); free(gt); return 1;
    }
    cfg.runtime.top_k = k;
    cfg.runtime.sync_only = 1;

    /* Open layer at a temp dir. */
    char tmpdir[] = "/tmp/wavedb_bench_XXXXXX";
    if (!mkdtemp(tmpdir)) { perror("mkdtemp"); free(vectors); free(queries); free(gt); return 1; }
    int err = 0;
    vector_layer_t *vl = vector_layer_open_separate(tmpdir, "bench", &cfg, &err);
    if (!vl) {
        fprintf(stderr, "open_separate failed: %d\n", err);
        free(vectors); free(queries); free(gt); return 1;
    }

    /* Insert all vectors (timed). */
    double t0 = now_s();
    for (uint32_t i = 0; i < count; i++) {
        char id[32];
        snprintf(id, sizeof(id), "v%u", i);
        int rc = vector_layer_insert_sync(vl, id, vectors + (size_t)i * dim, NULL, 0);
        if (rc != 0) {
            fprintf(stderr, "insert %u failed: %d\n", i, rc);
            vector_layer_destroy(vl); free(vectors); free(queries); free(gt); return 1;
        }
    }
    double insert_s = now_s() - t0;
    double insert_ops = insert_s > 0 ? (double)count / insert_s : 0.0;

    /* Train (IVF/SLSH). */
    if (cfg.format.index_type != VL_INDEX_FLAT) {
        double tt0 = now_s();
        int rc = vector_layer_train(vl);
        double train_s = now_s() - tt0;
        if (rc != 0) fprintf(stderr, "train failed: %d\n", rc);
        fprintf(stderr, "train: %.2fs\n", train_s);
    }

    /* Search all queries (per-query latency). */
    double *latencies = malloc((size_t)qcount * sizeof(double));
    if (!latencies) { fprintf(stderr, "OOM latencies\n"); vector_layer_destroy(vl); free(vectors); free(queries); free(gt); return 1; }
    int total_hits = 0;
    double search_total = 0;
    for (uint32_t q = 0; q < qcount; q++) {
        double l0 = now_s();
        vl_result_t *results = NULL;
        int n = 0;
        int rc = vector_layer_search_sync(vl, queries + (size_t)q * dim, k, &results, &n);
        latencies[q] = now_s() - l0;
        search_total += latencies[q];
        if (rc != 0) { fprintf(stderr, "search %u failed: %d\n", q, rc); continue; }
        /* recall@10: count results in gt[q]. */
        for (int i = 0; i < n && i < 10; i++) {
            if (results[i].id == NULL) continue;
            /* ids are "v%u" — parse the integer after 'v'. */
            uint32_t rid = (uint32_t)strtoul(results[i].id + 1, NULL, 10);
            for (int j = 0; j < 10; j++) {
                if (gt[q * 10 + j] == (int32_t)rid) { total_hits++; break; }
            }
        }
        vector_layer_free_results(results, n);
    }
    double recall = (qcount > 0) ? (double)total_hits / ((double)qcount * 10.0) : 0.0;

    /* p50/p99 latency. */
    qsort(latencies, qcount, sizeof(double), dbl_cmp);
    double p50 = latencies[qcount / 2];
    double p99 = latencies[(qcount * 99) / 100];

    /* Storage per vector: db dir size / count. */
    long db_bytes = dir_size(tmpdir);
    double storage_per_vec = (count > 0) ? (double)db_bytes / (double)count : 0.0;

    /* Output JSON (single line). */
    printf("{\"corpus\":\"%s\",\"index\":\"%s\",\"k\":%d,\"dim\":%u,\"count\":%u,\"qcount\":%u,",
           corpus, itype, k, dim, count, qcount);
    printf("\"recall@10\":%.4f,\"insert_ops\":%.0f,\"insert_s\":%.2f,\"search_total_s\":%.2f,",
           recall, insert_ops, insert_s, search_total);
    printf("\"p50_ms\":%.3f,\"p99_ms\":%.3f,\"storage_per_vector_bytes\":%.0f}\n",
           p50 * 1000.0, p99 * 1000.0, storage_per_vec);

    /* Cleanup. */
    vector_layer_destroy(vl);
    free(vectors); free(queries); free(gt); free(latencies);
    char rmcmd[300];
    snprintf(rmcmd, sizeof(rmcmd), "rm -rf %s", tmpdir);
    system(rmcmd);
    return 0;
}