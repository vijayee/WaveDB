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

                raw_result_t* results = NULL;
                size_t count = 0;
                int rc = database_scan_sync_raw(layer->db, prefix.data, prefix.length - 1, '/', &results, &count);
                vec_deinit(&prefix);

                graph_pred_stats_t ps;
                ps.predicate = strdup(field->name);
                ps.triple_count = 0;
                ps.distinct_subjects = 0;

                if (rc == 0 && results) {
                    // Count distinct subjects from PSO results
                    // Key format: /pso/<predicate>/<subject>/<object>/
                    // We need to count unique subjects (3rd component)
                    vertex_set_t subjects;
                    vertex_set_init(&subjects, count > 0 ? count : 8);

                    // Build prefix for filtering: /pso/<predicate>/
                    vec_char_t pfx;
                    vec_init(&pfx);
                    vec_pusharr(&pfx, "/pso/", 5);
                    vec_pusharr(&pfx, field->name, strlen(field->name));
                    vec_pusharr(&pfx, "/", 1);
                    vec_push(&pfx, '\0');

                    for (size_t k = 0; k < count; k++) {
                        // Filter out results that don't match our prefix
                        size_t pi = 0;
                        size_t ki = 0;
                        size_t pfx_len = pfx.length > 0 ? (size_t)(pfx.length - 1) : 0;
                        int prefix_match = 1;
                        while (pi < pfx_len && ki < results[k].key_len) {
                            if (results[k].key[ki] == '\0') { ki++; continue; }
                            if (pfx.data[pi] != results[k].key[ki]) { prefix_match = 0; break; }
                            pi++; ki++;
                        }
                        if (!prefix_match) continue;

                        char buf[4096];
                        const char* subj = key_nth_component(results[k].key, results[k].key_len, 2, buf, sizeof(buf));
                        if (subj) vertex_set_add(&subjects, subj);
                    }

                    vec_deinit(&pfx);

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
            // Average fanout = triples / subjects
            return input_size * (triples / subjects);
        }
    }

    return input_size * 5; // default fanout estimate
}

size_t graph_stats_estimate_in(graph_stats_t* stats, const char* predicate, size_t input_size) {
    // In has the same cardinality as Out for symmetric predicates
    return graph_stats_estimate_out(stats, predicate, input_size);
}