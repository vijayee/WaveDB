//
// Created by victor on 06/01/26.
//
// Query optimizer: transforms the step chain to eliminate
// redundant operations and fuse filter+traversal patterns.
//

#include "graph_internal.h"
#include "../../Util/allocator.h"

/* ── Pass 1: Fuse consecutive Has+Out/In into a single step ──
 *
 * Pattern:  HAS(pred, val) → OUT(edge)  becomes  OUT(edge) with filter
 * Pattern:  HAS(pred, val) → IN(edge)   becomes  IN(edge) with filter
 *
 * The Has step is removed from the chain and its parameters are
 * attached to the Out/In step as filter fields.
 */
static int fuse_has_traversal(query_step_t** steps) {
    if (!steps || !*steps) return 0;

    query_step_t* s = *steps;
    query_step_t* prev = NULL;

    while (s && s->next) {
        if (s->type == GRAPH_STEP_HAS &&
            (s->next->type == GRAPH_STEP_OUT || s->next->type == GRAPH_STEP_IN)) {
            query_step_t* has_step = s;
            query_step_t* trav_step = s->next;

            // Move Has parameters to the traversal step
            trav_step->has_predicate = has_step->has_predicate;
            trav_step->has_value = has_step->has_value;
            trav_step->has_cmp = has_step->has_cmp;

            // Clear Has step fields (prevent double-free)
            has_step->has_predicate = NULL;
            has_step->has_value = NULL;

            // Remove Has step from the chain
            if (prev) prev->next = trav_step;
            else *steps = trav_step;

            // Free the Has step (fields already transferred)
            has_step->next = NULL;
            free(has_step);

            s = trav_step;
            // Don't advance prev — we might have another Has before this step
            // Actually, we just fused, so continue from trav_step
            continue;
        }
        prev = s;
        s = s->next;
    }

    return 0;
}

/* ── Pass 2: Fold single-child INTERSECT/UNION ── */

static int fold_single_child_ops(query_step_t** steps) {
    if (!steps || !*steps) return 0;

    query_step_t* s = *steps;
    query_step_t* prev = NULL;

    while (s) {
        query_step_t* next = s->next;

        // Fold single-child INTERSECT/UNION: inline the child chain
        if (s->children.length == 1 &&
            (s->type == GRAPH_STEP_INTERSECT || s->type == GRAPH_STEP_UNION)) {

            query_step_t* child = s->children.data[0];

            // VERTEX only works as the first step of a (sub-)chain.
            if (prev && child->type == GRAPH_STEP_VERTEX) {
                prev = s;
                s = next;
                continue;
            }

            // Find the tail of the child chain
            query_step_t* child_tail = child;
            while (child_tail->next) child_tail = child_tail->next;

            // Splice: prev → child...child_tail → s->next
            if (prev) prev->next = child;
            else *steps = child;
            child_tail->next = s->next;

            // Free the wrapper step (not the child chain)
            vec_deinit(&s->children);
            free(s);

            s = child;
        } else {
            prev = s;
            s = next;
        }
    }

    return 0;
}

int graph_optimize(query_step_t** steps) {
    if (!steps || !*steps) return 0;

    fuse_has_traversal(steps);
    fold_single_child_ops(steps);

    return 0;
}

/* ── Pass 3: Reorder consecutive Has steps by selectivity ──
 *
 * Collects consecutive HAS steps and sorts them by estimated
 * cardinality (most selective first). This is safe because
 * consecutive Has steps commute: Has("p1","v1").Has("p2","v2")
 * produces the same result as Has("p2","v2").Has("p1","v1").
 *
 * Requires stats to be computed. If stats are not available,
 * this pass is a no-op.
 */
int graph_optimize_reorder_has(query_step_t** steps, graph_layer_t* layer) {
    if (!steps || !*steps || !layer) return 0;
    if (!layer->stats || !layer->stats_computed) {
        // Lazily compute stats on first optimization pass
        graph_stats_compute(layer);
    }
    if (!layer->stats) return 0;

    query_step_t* s = *steps;
    while (s) {
        // Find a run of consecutive HAS steps
        if (s->type != GRAPH_STEP_HAS) {
            s = s->next;
            continue;
        }

        // Collect the run
        query_step_t* run_start = s;
        query_step_t* run_end = s;
        size_t run_len = 1;
        while (run_end->next && run_end->next->type == GRAPH_STEP_HAS) {
            run_end = run_end->next;
            run_len++;
        }

        if (run_len < 2) {
            s = s->next;
            continue;
        }

        // Build an array of (step, estimated_cardinality) and sort by cardinality
        typedef struct {
            query_step_t* step;
            size_t estimate;
        } has_est_t;

        has_est_t* estimates = (has_est_t*)get_memory(run_len * sizeof(has_est_t));
        query_step_t* cur = run_start;
        for (size_t i = 0; i < run_len; i++) {
            estimates[i].step = cur;
            estimates[i].estimate = graph_stats_estimate_has(
                layer->stats, cur->has_predicate, cur->has_value, cur->has_cmp);
            cur = cur->next;
        }

        // Sort by estimate (ascending = most selective first)
        for (size_t i = 0; i < run_len - 1; i++) {
            for (size_t j = i + 1; j < run_len; j++) {
                if (estimates[j].estimate < estimates[i].estimate) {
                    has_est_t tmp = estimates[i];
                    estimates[i] = estimates[j];
                    estimates[j] = tmp;
                }
            }
        }

        // Re-link the steps in sorted order
        // Save the predecessor and successor of the run
        // (we need to relink the entire run)
        for (size_t i = 0; i < run_len - 1; i++) {
            estimates[i].step->next = estimates[i + 1].step;
        }
        estimates[run_len - 1].step->next = run_end->next;

        // Fix the predecessor link
        // Find the step before run_start in the chain
        if (run_start == *steps) {
            *steps = estimates[0].step;
        } else {
            query_step_t* prev = *steps;
            while (prev && prev->next != run_start) prev = prev->next;
            if (prev) prev->next = estimates[0].step;
        }

        s = estimates[run_len - 1].step->next;
        free(estimates);
    }

    return 0;
}

/* ── Pass 4: Reorder INTERSECT children by cardinality ──
 *
 * For INTERSECT steps with multiple children, sort the children
 * array by estimated cardinality (ascending = most selective first).
 * This is safe because intersection is commutative and associative.
 * Executing the most selective child first produces a smaller
 * accumulator, reducing the cost of subsequent intersections.
 */
static void reorder_intersect_children_in_step(query_step_t* s, graph_layer_t* layer) {
    if (!s || s->type != GRAPH_STEP_INTERSECT) return;
    if (s->children.length < 2) return;
    if (!layer->stats) return;

    typedef struct {
        size_t index;
        size_t estimate;
    } child_est_t;

    child_est_t* estimates = (child_est_t*)get_memory(s->children.length * sizeof(child_est_t));
    for (size_t ci = 0; ci < (size_t)s->children.length; ci++) {
        estimates[ci].index = ci;
        estimates[ci].estimate = graph_stats_estimate_chain(layer->stats, s->children.data[ci]);
    }

    // Sort by estimate (ascending = most selective first)
    for (size_t i = 0; i < (size_t)s->children.length - 1; i++) {
        for (size_t j = i + 1; j < (size_t)s->children.length; j++) {
            if (estimates[j].estimate < estimates[i].estimate) {
                child_est_t tmp = estimates[i];
                estimates[i] = estimates[j];
                estimates[j] = tmp;
            }
        }
    }

    // Reorder children.data[] pointers in place
    query_step_t** new_order = (query_step_t**)get_memory(s->children.length * sizeof(query_step_t*));
    for (size_t i = 0; i < (size_t)s->children.length; i++) {
        new_order[i] = s->children.data[estimates[i].index];
    }
    memcpy(s->children.data, new_order, s->children.length * sizeof(query_step_t*));
    free(new_order);
    free(estimates);
}

int graph_optimize_reorder_intersect_children(query_step_t** steps, graph_layer_t* layer) {
    if (!steps || !*steps || !layer) return 0;
    if (!layer->stats || !layer->stats_computed) {
        graph_stats_compute(layer);
    }
    if (!layer->stats) return 0;

    // Walk the main chain and child sub-chains looking for INTERSECT steps
    query_step_t* s = *steps;
    while (s) {
        if (s->type == GRAPH_STEP_INTERSECT) {
            reorder_intersect_children_in_step(s, layer);

            // Also recurse into children to find nested INTERSECTs
            for (size_t ci = 0; ci < (size_t)s->children.length; ci++) {
                query_step_t* child = s->children.data[ci];
                while (child) {
                    if (child->type == GRAPH_STEP_INTERSECT) {
                        reorder_intersect_children_in_step(child, layer);
                    }
                    child = child->next;
                }
            }
        }
        s = s->next;
    }

    return 0;
}