//
// Created by victor on 05/30/26.
//

#include "graph_internal.h"
#include "../../Util/allocator.h"
#include <string.h>
#include <stdio.h>

/* ── Extract last path component from a reconstructed key ──
 *
 * Database scan keys are reconstructed from chunks and may contain
 * padding nulls (e.g., "spox" for 3-byte "spo" with chunk_size=4).
 * Use key_len for binary-safe extraction instead of strlen.
 */
static const char* key_last_component(const char* key, size_t key_len,
                                       char* buf, size_t buf_size) {
    if (!key || key_len == 0) return NULL;

    /* Find the last '/' in the key */
    const char* last_slash = NULL;
    for (size_t i = 0; i < key_len; i++) {
        if (key[i] == '/') last_slash = key + i;
    }
    if (!last_slash) return NULL;

    size_t comp_len = key_len - (size_t)(last_slash - key) - 1;
    if (comp_len >= buf_size) comp_len = buf_size - 1;

    memcpy(buf, last_slash + 1, comp_len);
    buf[comp_len] = '\0';
    return buf;
}

/* ── Check if a reconstructed key starts with a full prefix ──
 *
 * Database scans return all keys from the prefix onward (no upper bound),
 * so we must filter results ourselves. This checks a multi-component
 * prefix such as "/spo/clip_abc/tagged_with/".
 *
 * The prefix should include the leading '/' (e.g. "/spo/clip_abc/tagged_with/").
 * Reconstructed keys do NOT have a leading '/', so we skip prefix[0].
 *
 * Null-padding bytes in the key are skipped during comparison.
 */
static int key_starts_with_prefix(const char* key, size_t key_len,
                                   const char* prefix) {
    if (!key || !prefix) return 0;
    size_t prefix_len = strlen(prefix);
    if (prefix_len < 2) return 0;   /* need at least '/' + char */

    size_t ki = 0;
    /* Skip leading '/' in prefix (key has no leading '/') */
    for (size_t pi = 1; pi < prefix_len; pi++) {
        /* Skip padding nulls in the key */
        while (ki < key_len && key[ki] == '\0') ki++;
        if (ki >= key_len) return 0;
        if (key[ki] != prefix[pi]) return 0;
        ki++;
    }
    return 1;
}

/* ── SPO scan: /spo/<subject>/<predicate>/ → collect objects ── */

int graph_execute_out(database_t* db, const vertex_set_t* input,
                       const char* predicate, vertex_set_t* output) {
    if (!db || !input || !predicate || !output) return -1;

    for (size_t i = 0; i < input->count; i++) {
        char prefix[1024];
        int len = snprintf(prefix, sizeof(prefix), "/spo/%s/%s/", input->vertices[i], predicate);
        if (len < 0 || (size_t)len >= sizeof(prefix)) continue; // Truncated: skip (path > 1024)

        raw_result_t* results = NULL;
        size_t count = 0;
        int rc = database_scan_sync_raw(db, prefix, strlen(prefix), '/', &results, &count);
        if (rc != 0) continue;

        for (size_t j = 0; j < count; j++) {
            /* Filter: key must start with expected prefix */
            if (!key_starts_with_prefix(results[j].key, results[j].key_len, prefix))
                continue;

            char buf[1024];
            const char* obj = key_last_component(results[j].key, results[j].key_len,
                                                  buf, sizeof(buf));
            if (obj) vertex_set_add(output, obj);
        }
        database_raw_results_free(results, count);
    }
    return 0;
}

/* ── POS scan: /pos/<predicate>/<object>/ → collect subjects ── */

int graph_execute_in(database_t* db, const vertex_set_t* input,
                      const char* predicate, vertex_set_t* output) {
    if (!db || !input || !predicate || !output) return -1;

    for (size_t i = 0; i < input->count; i++) {
        char prefix[1024];
        int len = snprintf(prefix, sizeof(prefix), "/pos/%s/%s/", predicate, input->vertices[i]);
        if (len < 0 || (size_t)len >= sizeof(prefix)) continue; // Truncated: skip (path > 1024)

        raw_result_t* results = NULL;
        size_t count = 0;
        int rc = database_scan_sync_raw(db, prefix, strlen(prefix), '/', &results, &count);
        if (rc != 0) continue;

        for (size_t j = 0; j < count; j++) {
            /* Filter: key must start with expected prefix */
            if (!key_starts_with_prefix(results[j].key, results[j].key_len, prefix))
                continue;

            char buf[1024];
            const char* subj = key_last_component(results[j].key, results[j].key_len,
                                                   buf, sizeof(buf));
            if (subj) vertex_set_add(output, subj);
        }
        database_raw_results_free(results, count);
    }
    return 0;
}

/* ── Has scan: POS scan for (predicate, value), intersect with input ── */

int graph_execute_has(database_t* db, const vertex_set_t* input,
                       const char* predicate, const char* value,
                       vertex_set_t* output) {
    if (!db || !input || !predicate || !value || !output) return -1;

    char prefix[1024];
    int len = snprintf(prefix, sizeof(prefix), "/pos/%s/%s/", predicate, value);
    if (len < 0 || (size_t)len >= sizeof(prefix)) return -1;

    raw_result_t* results = NULL;
    size_t count = 0;
    int rc = database_scan_sync_raw(db, prefix, strlen(prefix), '/', &results, &count);
    if (rc != 0) return 0; // No results = no match

    vertex_set_t pos_results;
    vertex_set_init(&pos_results, count > 0 ? count : 8);
    for (size_t j = 0; j < count; j++) {
        if (!key_starts_with_prefix(results[j].key, results[j].key_len, prefix))
            continue;
        char buf[1024];
        const char* subj = key_last_component(results[j].key, results[j].key_len, buf, sizeof(buf));
        if (subj) vertex_set_add(&pos_results, subj);
    }
    database_raw_results_free(results, count);

    // If input is empty, Has is the starting point — just return POS results
    if (input->count == 0) {
        vertex_set_copy(output, &pos_results);
    } else {
        vertex_set_intersect(output, input, &pos_results);
    }
    vertex_set_destroy(&pos_results);
    return 0;
}

/* ── Vertex step: produce a singleton set ── */

int graph_execute_vertex(database_t* db, query_step_t* step, vertex_set_t* output) {
    (void)db;
    if (!step || !step->vertex_id || !output) return -1;
    vertex_set_add(output, step->vertex_id);
    return 0;
}

/* ── Step chain execution ── */

// Execute a list of child steps (for INTERSECT/UNION children).
// Each child list starts from nothing (Vertex step typically).
static int execute_child_steps(database_t* db, query_step_t* steps, vertex_set_t* output) {
    vertex_set_t current;
    vertex_set_init(&current, 8);
    int first = 1;

    query_step_t* s = steps;
    while (s) {
        vertex_set_t next;
        vertex_set_init(&next, 8);

        if (s->type == GRAPH_STEP_VERTEX && first) {
            graph_execute_vertex(db, s, &next);
        } else if (s->type == GRAPH_STEP_OUT) {
            graph_execute_out(db, &current, s->predicate, &next);
        } else if (s->type == GRAPH_STEP_IN) {
            graph_execute_in(db, &current, s->predicate, &next);
        } else if (s->type == GRAPH_STEP_LIMIT) {
            vertex_set_copy(&next, &current);
            if (next.count > s->limit) {
                next.count = s->limit;
            }
        } else if (s->type == GRAPH_STEP_HAS) {
            if (first) {
                // Has as starting point: POS scan for (predicate, value)
                vertex_set_t empty;
                vertex_set_init(&empty, 0);
                graph_execute_has(db, &empty, s->has_predicate, s->has_value, &next);
                vertex_set_destroy(&empty);
            } else {
                graph_execute_has(db, &current, s->has_predicate, s->has_value, &next);
            }
        } else if (s->type == GRAPH_STEP_INTERSECT || s->type == GRAPH_STEP_UNION) {
            vertex_set_t combined;
            vertex_set_init(&combined, 8);
            int first_child = 1;
            for (size_t ci = 0; ci < s->num_children; ci++) {
                vertex_set_t child_result;
                vertex_set_init(&child_result, 8);
                execute_child_steps(db, s->children[ci], &child_result);
                if (first_child) {
                    vertex_set_copy(&combined, &child_result);
                    first_child = 0;
                } else {
                    vertex_set_t tmp;
                    vertex_set_init(&tmp, 8);
                    if (s->type == GRAPH_STEP_INTERSECT) {
                        vertex_set_intersect(&tmp, &combined, &child_result);
                    } else {
                        vertex_set_union(&tmp, &combined, &child_result);
                    }
                    vertex_set_destroy(&combined);
                    memcpy(&combined, &tmp, sizeof(vertex_set_t));
                }
                vertex_set_destroy(&child_result);
            }
            if (current.count > 0) {
                vertex_set_t tmp;
                vertex_set_init(&tmp, 8);
                if (s->type == GRAPH_STEP_INTERSECT) {
                    vertex_set_intersect(&tmp, &current, &combined);
                } else {
                    vertex_set_union(&tmp, &current, &combined);
                }
                vertex_set_destroy(&combined);
                memcpy(&combined, &tmp, sizeof(vertex_set_t));
            }
            vertex_set_copy(&next, &combined);
            vertex_set_destroy(&combined);
        }

        vertex_set_destroy(&current);
        memcpy(&current, &next, sizeof(vertex_set_t));
        s = s->next;
        first = 0;
    }

    vertex_set_copy(output, &current);
    vertex_set_destroy(&current);
    return 0;
}

int graph_execute_chain(database_t* db, query_step_t* steps, vertex_set_t* output) {
    if (!db || !steps || !output) return -1;
    return execute_child_steps(db, steps, output);
}
