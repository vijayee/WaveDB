//
// Created by victor on 05/30/26.
//

#include "graph_internal.h"
#include "../../Util/allocator.h"
#include "../../Util/vec.h"
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

/* ── Extract the Nth path component from a reconstructed key ──
 *
 * Keys are structured as /comp0/comp1/comp2/...
 * Component 0 is the index name (e.g., "pos"), component 1 is the
 * first field, etc. Returns the component string in buf, or NULL.
 * Skips padding nulls in the key.
 */
const char* key_nth_component(const char* key, size_t key_len, int n,
                              char* buf, size_t buf_size) {
    if (!key || key_len == 0 || n < 0) return NULL;

    int component = 0;
    size_t start = 0;
    size_t i = 0;

    /* Skip leading '/' */
    if (i < key_len && key[i] == '/') i++;

    while (i < key_len && component < n) {
        /* Find end of current component */
        start = i;
        while (i < key_len && key[i] != '/' && key[i] != '\0') i++;
        component++;
        if (component <= n) {
            /* Skip the separator */
            while (i < key_len && (key[i] == '/' || key[i] == '\0')) i++;
        }
    }

    if (component < n) return NULL;

    /* Extract component n */
    start = i;
    size_t end = i;
    while (end < key_len && key[end] != '/' && key[end] != '\0') end++;

    size_t comp_len = end - start;
    if (comp_len == 0 || comp_len >= buf_size) return NULL;

    memcpy(buf, key + start, comp_len);
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

    int had_error = 0;
    for (size_t i = 0; i < input->count; i++) {
        vec_char_t prefix;
        vec_init(&prefix);
        build_prefix_vec(&prefix, "spo", input->vertices[i], predicate);

        raw_result_t* results = NULL;
        size_t count = 0;
        int rc = database_scan_sync_raw(db, prefix.data, prefix.length - 1, '/', &results, &count);
        if (rc != 0) {
            had_error = rc;
            vec_deinit(&prefix);
            continue;
        }

        for (size_t j = 0; j < count; j++) {
            /* Filter: key must start with expected prefix */
            if (!key_starts_with_prefix(results[j].key, results[j].key_len, prefix.data))
                continue;

            char buf[4096];
            const char* obj = key_last_component(results[j].key, results[j].key_len,
                                                  buf, sizeof(buf));
            if (obj) vertex_set_add(output, obj);
        }
        database_raw_results_free(results, count);
        vec_deinit(&prefix);
    }
    return had_error;
}

/* ── POS scan: /pos/<predicate>/<object>/ → collect subjects ── */

int graph_execute_in(database_t* db, const vertex_set_t* input,
                      const char* predicate, vertex_set_t* output) {
    if (!db || !input || !predicate || !output) return -1;

    int had_error = 0;
    for (size_t i = 0; i < input->count; i++) {
        vec_char_t prefix;
        vec_init(&prefix);
        build_prefix_vec(&prefix, "pos", predicate, input->vertices[i]);

        raw_result_t* results = NULL;
        size_t count = 0;
        int rc = database_scan_sync_raw(db, prefix.data, prefix.length - 1, '/', &results, &count);
        if (rc != 0) {
            had_error = rc;
            vec_deinit(&prefix);
            continue;
        }

        for (size_t j = 0; j < count; j++) {
            /* Filter: key must start with expected prefix */
            if (!key_starts_with_prefix(results[j].key, results[j].key_len, prefix.data))
                continue;

            char buf[4096];
            const char* subj = key_last_component(results[j].key, results[j].key_len,
                                                   buf, sizeof(buf));
            if (subj) vertex_set_add(output, subj);
        }
        database_raw_results_free(results, count);
        vec_deinit(&prefix);
    }
    return had_error;
}

/* ── Compare two strings using a graph comparison operator ── */

static int cmp_strings(const char* a, const char* b, graph_cmp_op_t cmp) {
    int c = strcmp(a, b);
    switch (cmp) {
        case GRAPH_CMP_EQ:  return c == 0;
        case GRAPH_CMP_GT:  return c > 0;
        case GRAPH_CMP_GTE: return c >= 0;
        case GRAPH_CMP_LT:  return c < 0;
        case GRAPH_CMP_LTE: return c <= 0;
    }
    return 0;
}

/* ── Has scan: POS scan for (predicate, value [cmp]), intersect with input ── */

int graph_execute_has(database_t* db, const vertex_set_t* input,
                       const char* predicate, const char* value,
                       graph_cmp_op_t cmp, vertex_set_t* output) {
    if (!db || !input || !predicate || !value || !output) return -1;

    if (cmp == GRAPH_CMP_EQ) {
        /* Equality: exact prefix scan /pos/<predicate>/<value>/ */
        vec_char_t prefix;
        vec_init(&prefix);
        build_prefix_vec(&prefix, "pos", predicate, value);

        raw_result_t* results = NULL;
        size_t count = 0;
        int rc = database_scan_sync_raw(db, prefix.data, prefix.length - 1, '/', &results, &count);
        if (rc != 0) {
            vec_deinit(&prefix);
            return rc;
        }

        for (size_t j = 0; j < count; j++) {
            if (!key_starts_with_prefix(results[j].key, results[j].key_len, prefix.data))
                continue;
            char buf[4096];
            const char* subj = key_last_component(results[j].key, results[j].key_len, buf, sizeof(buf));
            if (!subj) continue;
            if (input->count == 0 || vertex_set_contains((vertex_set_t*)input, subj)) {
                vertex_set_add(output, subj);
            }
        }
        database_raw_results_free(results, count);
        vec_deinit(&prefix);
    } else {
        /* Range: scan all /pos/<predicate>/ entries, filter by object component */
        vec_char_t prefix;
        vec_init(&prefix);
        vec_pusharr(&prefix, "/pos/", 5);
        vec_pusharr(&prefix, predicate, strlen(predicate));
        vec_pusharr(&prefix, "/", 1);
        vec_push(&prefix, '\0');

        raw_result_t* results = NULL;
        size_t count = 0;
        int rc = database_scan_sync_raw(db, prefix.data, prefix.length - 1, '/', &results, &count);
        if (rc != 0) {
            vec_deinit(&prefix);
            return rc;
        }

        for (size_t j = 0; j < count; j++) {
            if (!key_starts_with_prefix(results[j].key, results[j].key_len, prefix.data))
                continue;
            char obj_buf[4096];
            const char* obj = key_nth_component(results[j].key, results[j].key_len, 2, obj_buf, sizeof(obj_buf));
            if (!obj) continue;
            if (!cmp_strings(obj, value, cmp)) continue;

            char subj_buf[4096];
            const char* subj = key_last_component(results[j].key, results[j].key_len, subj_buf, sizeof(subj_buf));
            if (!subj) continue;
            if (input->count == 0 || vertex_set_contains((vertex_set_t*)input, subj)) {
                vertex_set_add(output, subj);
            }
        }
        database_raw_results_free(results, count);
        vec_deinit(&prefix);
    }

    return 0;
}

/* ── Fused Has+Out: POS scan for (predicate, value) filter, then SPO expansion ── */

static int graph_execute_has_out(database_t* db, const vertex_set_t* input,
                                const char* has_predicate, const char* has_value,
                                graph_cmp_op_t has_cmp,
                                const char* out_predicate, vertex_set_t* output) {
    if (!db || !out_predicate || !output) return -1;

    /* Phase 1: collect vertices matching Has filter */
    vertex_set_t has_results;
    vertex_set_init(&has_results, 16);
    int rc = graph_execute_has(db, input, has_predicate, has_value, has_cmp, &has_results);
    if (rc != 0) {
        vertex_set_destroy(&has_results);
        return rc;
    }

    /* Phase 2: expand each matching vertex via Out */
    for (size_t i = 0; i < has_results.count; i++) {
        vertex_set_t one;
        vertex_set_init(&one, 1);
        vertex_set_add(&one, has_results.vertices[i]);
        vertex_set_t next;
        vertex_set_init(&next, 8);
        int r = graph_execute_out(db, &one, out_predicate, &next);
        if (r != 0) rc = r;
        /* Merge next into output */
        for (size_t j = 0; j < next.count; j++) {
            vertex_set_add(output, next.vertices[j]);
        }
        vertex_set_destroy(&next);
        vertex_set_destroy(&one);
    }
    vertex_set_destroy(&has_results);
    return rc;
}

/* ── Fused Has+In: POS scan for (predicate, value) filter, then POS expansion ── */

static int graph_execute_has_in(database_t* db, const vertex_set_t* input,
                               const char* has_predicate, const char* has_value,
                               graph_cmp_op_t has_cmp,
                               const char* in_predicate, vertex_set_t* output) {
    if (!db || !in_predicate || !output) return -1;

    /* Phase 1: collect vertices matching Has filter */
    vertex_set_t has_results;
    vertex_set_init(&has_results, 16);
    int rc = graph_execute_has(db, input, has_predicate, has_value, has_cmp, &has_results);
    if (rc != 0) {
        vertex_set_destroy(&has_results);
        return rc;
    }

    /* Phase 2: expand each matching vertex via In */
    for (size_t i = 0; i < has_results.count; i++) {
        vertex_set_t one;
        vertex_set_init(&one, 1);
        vertex_set_add(&one, has_results.vertices[i]);
        vertex_set_t next;
        vertex_set_init(&next, 8);
        int r = graph_execute_in(db, &one, in_predicate, &next);
        if (r != 0) rc = r;
        for (size_t j = 0; j < next.count; j++) {
            vertex_set_add(output, next.vertices[j]);
        }
        vertex_set_destroy(&next);
        vertex_set_destroy(&one);
    }
    vertex_set_destroy(&has_results);
    return rc;
}

/* ── OSP scan: /osp/<object>/<subject>/ → collect predicates ── */

int graph_execute_osp(database_t* db, const vertex_set_t* input,
                       const char* object, vertex_set_t* output) {
    if (!db || !input || !object || !output) return -1;

    int had_error = 0;
    for (size_t i = 0; i < input->count; i++) {
        vec_char_t prefix;
        vec_init(&prefix);
        build_prefix_vec(&prefix, "osp", object, input->vertices[i]);

        raw_result_t* results = NULL;
        size_t count = 0;
        int rc = database_scan_sync_raw(db, prefix.data, prefix.length - 1, '/', &results, &count);
        if (rc != 0) {
            had_error = rc;
            vec_deinit(&prefix);
            continue;
        }

        for (size_t j = 0; j < count; j++) {
            if (!key_starts_with_prefix(results[j].key, results[j].key_len, prefix.data))
                continue;
            char buf[4096];
            const char* pred = key_last_component(results[j].key, results[j].key_len, buf, sizeof(buf));
            if (pred) vertex_set_add(output, pred);
        }
        database_raw_results_free(results, count);
        vec_deinit(&prefix);
    }
    return had_error;
}

/* ── PSO scan: /pso/<predicate>/ → collect subjects ── */

int graph_execute_pso(database_t* db, const char* predicate, vertex_set_t* output) {
    if (!db || !predicate || !output) return -1;

    vec_char_t prefix;
    vec_init(&prefix);
    vec_pusharr(&prefix, "/pso/", 5);
    vec_pusharr(&prefix, predicate, strlen(predicate));
    vec_pusharr(&prefix, "/", 1);
    vec_push(&prefix, '\0');

    raw_result_t* results = NULL;
    size_t count = 0;
    int rc = database_scan_sync_raw(db, prefix.data, prefix.length - 1, '/', &results, &count);
    if (rc != 0) {
        vec_deinit(&prefix);
        return rc;
    }

    for (size_t j = 0; j < count; j++) {
        if (!key_starts_with_prefix(results[j].key, results[j].key_len, prefix.data))
            continue;
        char buf[4096];
        const char* subj = key_last_component(results[j].key, results[j].key_len, buf, sizeof(buf));
        if (subj) vertex_set_add(output, subj);
    }
    database_raw_results_free(results, count);
    vec_deinit(&prefix);
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

// Execute a list of child steps (for INTERSECT/UNION/DIFFERENCE children).
// Each child list starts from nothing (Vertex step typically).
static int execute_child_steps(graph_layer_t* layer, query_step_t* steps, vertex_set_t* output) {
    if (!layer || !layer->db || !steps || !output) return -1;
    database_t* db = layer->db;
    vertex_set_t current;
    vertex_set_init(&current, 8);
    int first = 1;
    int error = 0;

    query_step_t* s = steps;
    while (s) {
        vertex_set_t next;
        vertex_set_init(&next, 8);

        if (s->type == GRAPH_STEP_VERTEX && first) {
            int rc = graph_execute_vertex(db, s, &next);
            if (rc != 0) error = rc;
        } else if (s->type == GRAPH_STEP_OUT) {
            int rc;
            if (s->has_predicate) {
                rc = graph_execute_has_out(db, &current, s->has_predicate, s->has_value, s->has_cmp, s->predicate, &next);
            } else {
                rc = graph_execute_out(db, &current, s->predicate, &next);
            }
            if (rc != 0) error = rc;
        } else if (s->type == GRAPH_STEP_IN) {
            int rc;
            if (s->has_predicate) {
                rc = graph_execute_has_in(db, &current, s->has_predicate, s->has_value, s->has_cmp, s->predicate, &next);
            } else {
                rc = graph_execute_in(db, &current, s->predicate, &next);
            }
            if (rc != 0) error = rc;
        } else if (s->type == GRAPH_STEP_LIMIT) {
            vertex_set_destroy(&next);
            size_t copy_count = current.count < s->limit ? current.count : s->limit;
            vertex_set_init(&next, copy_count > 0 ? copy_count : 8);
            for (size_t _i = 0; _i < copy_count; _i++) {
                vertex_set_add(&next, current.vertices[_i]);
            }
        } else if (s->type == GRAPH_STEP_HAS) {
            int rc;
            if (first) {
                vertex_set_t empty;
                vertex_set_init(&empty, 0);
                rc = graph_execute_has(db, &empty, s->has_predicate, s->has_value, s->has_cmp, &next);
                vertex_set_destroy(&empty);
            } else {
                rc = graph_execute_has(db, &current, s->has_predicate, s->has_value, s->has_cmp, &next);
            }
            if (rc != 0) error = rc;
        } else if (s->type == GRAPH_STEP_MORPHISM) {
            // Look up the morphism by name, deep-copy its steps, execute them
            // against the current vertex set (extending the traversal chain)
            morphism_entry_t* found = NULL;
            for (size_t mi = 0; mi < (size_t)layer->morphisms.length; mi++) {
                if (strcmp(layer->morphisms.data[mi].name, s->morphism_name) == 0) {
                    found = &layer->morphisms.data[mi];
                    break;
                }
            }
            if (!found) {
                error = -1;
            } else if (found->steps) {
                query_step_t* copied = copy_steps(found->steps);
                // Execute each morphism step against a local current set,
                // seeded from the outer current vertex set
                vertex_set_t mcurrent;
                vertex_set_init(&mcurrent, current.count > 0 ? current.count : 8);
                vertex_set_copy(&mcurrent, &current);

                query_step_t* ms = copied;
                while (ms) {
                    vertex_set_t mnext;
                    vertex_set_init(&mnext, 8);

                    if (ms->type == GRAPH_STEP_VERTEX) {
                        graph_execute_vertex(db, ms, &mnext);
                    } else if (ms->type == GRAPH_STEP_OUT) {
                        graph_execute_out(db, &mcurrent, ms->predicate, &mnext);
                    } else if (ms->type == GRAPH_STEP_IN) {
                        graph_execute_in(db, &mcurrent, ms->predicate, &mnext);
                    } else if (ms->type == GRAPH_STEP_HAS) {
                        if (mcurrent.count == 0) {
                            vertex_set_t empty;
                            vertex_set_init(&empty, 0);
                            graph_execute_has(db, &empty, ms->has_predicate, ms->has_value, ms->has_cmp, &mnext);
                            vertex_set_destroy(&empty);
                        } else {
                            graph_execute_has(db, &mcurrent, ms->has_predicate, ms->has_value, ms->has_cmp, &mnext);
                        }
                    } else if (ms->type == GRAPH_STEP_LIMIT) {
                        vertex_set_destroy(&mnext);
                        size_t copy_count = mcurrent.count < ms->limit ? mcurrent.count : ms->limit;
                        vertex_set_init(&mnext, copy_count > 0 ? copy_count : 8);
                        for (size_t _i = 0; _i < copy_count; _i++) {
                            vertex_set_add(&mnext, mcurrent.vertices[_i]);
                        }
                    } else if (ms->type == GRAPH_STEP_INTERSECT || ms->type == GRAPH_STEP_UNION || ms->type == GRAPH_STEP_DIFFERENCE) {
                        vertex_set_t combined;
                        vertex_set_init(&combined, 8);
                        int first_child = 1;
                        for (size_t ci = 0; ci < (size_t)ms->children.length; ci++) {
                            vertex_set_t child_result;
                            vertex_set_init(&child_result, 8);
                            execute_child_steps(layer, ms->children.data[ci], &child_result);
                            if (first_child) {
                                vertex_set_copy(&combined, &child_result);
                                first_child = 0;
                            } else {
                                vertex_set_t tmp;
                                vertex_set_init(&tmp, 8);
                                if (ms->type == GRAPH_STEP_INTERSECT) {
                                    vertex_set_intersect(&tmp, &combined, &child_result);
                                } else if (ms->type == GRAPH_STEP_UNION) {
                                    vertex_set_union(&tmp, &combined, &child_result);
                                } else {
                                    vertex_set_difference(&tmp, &combined, &child_result);
                                }
                                vertex_set_destroy(&combined);
                                memcpy(&combined, &tmp, sizeof(vertex_set_t));
                            }
                            vertex_set_destroy(&child_result);
                        }
                        if (mcurrent.count > 0) {
                            vertex_set_t tmp;
                            vertex_set_init(&tmp, 8);
                            if (ms->type == GRAPH_STEP_INTERSECT) {
                                vertex_set_intersect(&tmp, &mcurrent, &combined);
                            } else if (ms->type == GRAPH_STEP_UNION) {
                                vertex_set_union(&tmp, &mcurrent, &combined);
                            } else {
                                vertex_set_difference(&tmp, &mcurrent, &combined);
                            }
                            vertex_set_destroy(&combined);
                            memcpy(&combined, &tmp, sizeof(vertex_set_t));
                        }
                        vertex_set_copy(&mnext, &combined);
                        vertex_set_destroy(&combined);
                    }

                    vertex_set_destroy(&mcurrent);
                    memcpy(&mcurrent, &mnext, sizeof(vertex_set_t));
                    ms = ms->next;
                }

                // Deep-free the copied steps
                query_step_t* cs = copied;
                while (cs) {
                    query_step_t* cn = cs->next;
                    free(cs->vertex_id);
                    free(cs->predicate);
                    free(cs->has_predicate);
                    free(cs->has_value);
                    free(cs->morphism_name);
                    if (cs->children.data) {
                        for (size_t ci = 0; ci < (size_t)cs->children.length; ci++) {
                            query_step_t* ch = cs->children.data[ci];
                            while (ch) {
                                query_step_t* chn = ch->next;
                                free(ch->vertex_id);
                                free(ch->predicate);
                                free(ch->has_predicate);
                                free(ch->has_value);
                                free(ch->morphism_name);
                                free(ch);
                                ch = chn;
                            }
                        }
                        vec_deinit(&cs->children);
                    }
                    free(cs);
                    cs = cn;
                }
                // Morphism result becomes 'next'
                vertex_set_copy(&next, &mcurrent);
                vertex_set_destroy(&mcurrent);
            }
        } else if (s->type == GRAPH_STEP_INTERSECT || s->type == GRAPH_STEP_UNION || s->type == GRAPH_STEP_DIFFERENCE) {
            vertex_set_t combined;
            vertex_set_init(&combined, 8);
            int first_child = 1;
            for (size_t ci = 0; ci < (size_t)s->children.length; ci++) {
                vertex_set_t child_result;
                vertex_set_init(&child_result, 8);
                execute_child_steps(layer, s->children.data[ci], &child_result);
                if (first_child) {
                    vertex_set_copy(&combined, &child_result);
                    first_child = 0;
                } else {
                    vertex_set_t tmp;
                    vertex_set_init(&tmp, 8);
                    if (s->type == GRAPH_STEP_INTERSECT) {
                        vertex_set_intersect(&tmp, &combined, &child_result);
                    } else if (s->type == GRAPH_STEP_UNION) {
                        vertex_set_union(&tmp, &combined, &child_result);
                    } else {
                        vertex_set_difference(&tmp, &combined, &child_result);
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
                } else if (s->type == GRAPH_STEP_UNION) {
                    vertex_set_union(&tmp, &current, &combined);
                } else {
                    // DIFFERENCE: current minus combined (the exclusion set)
                    vertex_set_difference(&tmp, &current, &combined);
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
    return error;
}

int graph_execute_chain(graph_layer_t* layer, query_step_t* steps, vertex_set_t* output) {
    if (!layer || !steps || !output) return -1;
    return execute_child_steps(layer, steps, output);
}
