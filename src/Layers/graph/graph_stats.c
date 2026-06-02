//
// Created by victor on 06/02/26.
//
// Cost-based query optimization statistics.
// Computes per-predicate cardinality estimates by scanning PSO indices.
//

#include "graph_internal.h"
#include "../../Util/allocator.h"
#include "../../Util/vec.h"
#include <string.h>

/* ── Compute statistics by scanning PSO indices ── */

int graph_stats_compute(graph_layer_t* layer) {
    if (!layer || !layer->db) return -1;
    if (layer->stats) return 0; // already computed
    // Caller must hold write-lock on layer->lock (writes layer->stats and layer->stats_computed)

    graph_stats_t* stats = (graph_stats_t*)get_clear_memory(sizeof(graph_stats_t));
    vec_init(&stats->predicates);

    // Count total unique subjects across all predicates via PSO scans
    // For each predicate, scan /pso/<predicate>/ and count distinct subjects
    // If no schema, scan all known PSO prefixes (not practical without schema)

    if (layer->schema) {
        for (size_t i = 0; i < (size_t)layer->schema->types.length; i++) {
            graph_schema_type_t* type = &layer->schema->types.data[i];
            for (size_t j = 0; j < (size_t)type->fields.length; j++) {
                graph_schema_field_t* field = &type->fields.data[j];

                // Only compute stats for predicates that have a POS or PSO index
                if (!(field->indices & (GRAPH_INDEX_POS | GRAPH_INDEX_PSO))) continue;

                // Check if we already have stats for this predicate
                int found = 0;
                for (size_t k = 0; k < (size_t)stats->predicates.length; k++) {
                    if (strcmp(stats->predicates.data[k].predicate, field->name) == 0) {
                        found = 1;
                        break;
                    }
                }
                if (found) continue;

                // Scan /pso/<predicate>/ to count distinct subjects
                vec_char_t prefix;
                vec_init(&prefix);
                vec_pusharr(&prefix, "/pso/", 5);
                vec_pusharr(&prefix, field->name, strlen(field->name));
                vec_pusharr(&prefix, "/", 1);
                vec_push(&prefix, '\0');

                size_t prefix_len = prefix.length - 1;
                char end_buf[4096];
                if (prefix_len + 2 > sizeof(end_buf)) { vec_deinit(&prefix); continue; }
                memcpy(end_buf, prefix.data, prefix_len);
                size_t end_len = append_successor(end_buf, prefix_len);

                raw_result_t* results = NULL;
                size_t count = 0;
                int rc = database_scan_range_sync_raw(layer->db, prefix.data, prefix.length - 1,
                                                       end_buf, end_len, '/', &results, &count);
                vec_deinit(&prefix);

                graph_pred_stats_t ps;
                ps.predicate = strdup(field->name);
                ps.triple_count = 0;
                ps.distinct_subjects = 0;

                if (rc == 0 && results) {
                    vertex_set_t subjects;
                    vertex_set_init(&subjects, count > 0 ? count : 8);

                    for (size_t k = 0; k < count; k++) {
                        char buf[4096];
                        const char* subj = key_nth_component(results[k].key, results[k].key_len, 2, buf, sizeof(buf));
                        if (subj) vertex_set_add(&subjects, subj);
                    }

                    ps.triple_count = count;
                    ps.distinct_subjects = subjects.count;
                    vertex_set_destroy(&subjects);
                    database_raw_results_free(results, count);
                }

                vec_push(&stats->predicates, ps);
            }
        }
    }

    layer->stats = stats;
    layer->stats_computed = 1;
    return 0;
}

/* ── Cardinality estimation ── */

size_t graph_stats_estimate_has(graph_stats_t* stats, const char* predicate,
                                 const char* value, graph_cmp_op_t cmp) {
    if (!stats) return 10; // default estimate

    // Find the predicate stats
    for (size_t i = 0; i < (size_t)stats->predicates.length; i++) {
        if (strcmp(stats->predicates.data[i].predicate, predicate) == 0) {
            size_t triples = stats->predicates.data[i].triple_count;
            size_t subjects = stats->predicates.data[i].distinct_subjects;
            if (subjects == 0) return 1;

            switch (cmp) {
                case GRAPH_CMP_EQ:
                    // Estimate: triples / subjects (average fanout per subject)
                    // This gives approximate matches per value
                    return subjects > 0 ? (triples + subjects - 1) / subjects : 1;
                case GRAPH_CMP_GT:
                case GRAPH_CMP_GTE:
                    // Rough estimate: half the subjects match a GT/GTE filter
                    return subjects / 2;
                case GRAPH_CMP_LT:
                case GRAPH_CMP_LTE:
                    return subjects / 2;
            }
        }
    }

    // Unknown predicate — conservative default
    return 10;
}

size_t graph_stats_estimate_out(graph_stats_t* stats, const char* predicate, size_t input_size) {
    if (!stats) return input_size * 5; // default fanout estimate

    for (size_t i = 0; i < (size_t)stats->predicates.length; i++) {
        if (strcmp(stats->predicates.data[i].predicate, predicate) == 0) {
            size_t triples = stats->predicates.data[i].triple_count;
            size_t subjects = stats->predicates.data[i].distinct_subjects;
            if (subjects == 0) return input_size;
            // Average fanout = triples / subjects (minimum 1)
            size_t fanout = subjects > 0 ? (triples + subjects - 1) / subjects : 1;
            return input_size > 0 && fanout > 0 ? input_size * fanout : input_size;
        }
    }

    return input_size * 5; // default fanout estimate
}

size_t graph_stats_estimate_in(graph_stats_t* stats, const char* predicate, size_t input_size) {
    // In has the same cardinality as Out for symmetric predicates
    return graph_stats_estimate_out(stats, predicate, input_size);
}

/* ── Estimate cardinality of an entire step chain ── */

size_t graph_stats_estimate_chain(graph_stats_t* stats, query_step_t* head) {
    if (!head) return 0;

    size_t current_size = 0;
    query_step_t* s = head;

    while (s) {
        switch (s->type) {
            case GRAPH_STEP_VERTEX:
                current_size = 1;
                break;
            case GRAPH_STEP_HAS:
                if (current_size == 0) {
                    // Has at the start seeds from the full POS scan
                    current_size = graph_stats_estimate_has(stats, s->has_predicate,
                                                            s->has_value, s->has_cmp);
                } else {
                    // Has as a filter reduces the set
                    size_t has_est = graph_stats_estimate_has(stats, s->has_predicate,
                                                              s->has_value, s->has_cmp);
                    // Conservative: estimate selectivity as min(current, has_matches)
                    if (has_est < current_size) current_size = has_est;
                }
                break;
            case GRAPH_STEP_OUT:
                current_size = graph_stats_estimate_out(stats, s->predicate, current_size);
                break;
            case GRAPH_STEP_IN:
                current_size = graph_stats_estimate_in(stats, s->predicate, current_size);
                break;
            case GRAPH_STEP_LIMIT:
                if (s->limit < current_size) current_size = s->limit;
                break;
            case GRAPH_STEP_INTERSECT: {
                // Estimate: min across all children
                size_t min_est = 0;
                int first = 1;
                for (size_t ci = 0; ci < (size_t)s->children.length; ci++) {
                    size_t child_est = graph_stats_estimate_chain(stats, s->children.data[ci]);
                    if (first || child_est < min_est) {
                        min_est = child_est;
                        first = 0;
                    }
                }
                current_size = min_est;
                break;
            }
            case GRAPH_STEP_UNION: {
                // Estimate: sum across all children (upper bound)
                size_t sum = 0;
                for (size_t ci = 0; ci < (size_t)s->children.length; ci++) {
                    sum += graph_stats_estimate_chain(stats, s->children.data[ci]);
                }
                current_size = sum;
                break;
            }
            case GRAPH_STEP_DIFFERENCE:
                // Estimate: size of first child (conservative upper bound)
                if (s->children.length > 0) {
                    current_size = graph_stats_estimate_chain(stats, s->children.data[0]);
                }
                break;
            case GRAPH_STEP_MORPHISM:
                // Cannot estimate without resolving the morphism — keep current
                break;
        }
        s = s->next;
    }

    return current_size > 0 ? current_size : 1;
}