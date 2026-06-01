//
// Created by victor on 06/01/26.
//
// Query optimizer: transforms the step chain to eliminate
// redundant operations before execution.
//

#include "graph_internal.h"
#include "../../Util/allocator.h"

int graph_optimize(query_step_t** steps) {
    if (!steps || !*steps) return 0;

    query_step_t* s = *steps;
    query_step_t* prev = NULL;

    while (s) {
        query_step_t* next = s->next;

        // Fold single-child INTERSECT/UNION: inline the child chain
        if (s->num_children == 1 &&
            (s->type == GRAPH_STEP_INTERSECT || s->type == GRAPH_STEP_UNION)) {

            query_step_t* child = s->children[0];

            // VERTEX only works as the first step of a (sub-)chain.
            // If we inline a VERTEX into a non-first position, it will
            // be silently skipped during execution (check for first==1).
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
            free(s->children);
            free(s);

            s = child;
        } else {
            prev = s;
            s = next;
        }
    }

    return 0;
}
