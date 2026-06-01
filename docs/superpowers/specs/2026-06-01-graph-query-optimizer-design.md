# Graph Query Optimizer Design

## Overview

A small optimizer pass that transforms the query step chain before execution. Runs between parsing and execution — takes the chain from `graph_query_t` and produces an optimized copy with no effect on the API.

## Optimizations

### 1. Eliminate Single-Child And/Or

`And(g.V("x").In("y"))` and `Or(g.V("x").In("y"))` produce `INTERSECT { children: [chain] }` / `UNION { children: [chain] }` with exactly one child. These are no-ops — a single-child intersect/union just returns the child's result unchanged.

**Transformation:** Replace the single-child INTERSECT/UNION step with its child chain inlined.

```
Before: VERTEX("x") → INTERSECT[chain: IN("y")] → LIMIT(5)
After:  VERTEX("x") → IN("y") → LIMIT(5)
```

### 2. Remove Empty Intersection Branches

If any INTERSECT child is a chain that we can statically determine produces an empty set, replace the INTERSECT with a constant empty set.

Currently only applies when the child is `VERTEX("nonexistent")` — at parse time we can't know if a vertex ID exists without scanning. This is a placeholder for future constant-folding.

### 3. Prune Unreachable Branches After Satisfied Limit

When `LIMIT(n)` appears before an INTERSECT/UNION, and the limit count was already reached by upstream steps, the downstream operations are irrelevant.

This can't be determined at optimization time (it depends on runtime data cardinality), so it's noted as a future consideration.

## Implementation

A single function `graph_optimize` that walks the step chain and applies transformations:

```c
int graph_optimize(query_step_t** steps);
```

The function:
1. Walks the chain from head to tail
2. For each step, applies applicable optimizations
3. Returns 0 on success (optimization never fails — best-effort)

**File:** New `graph_optimizer.c` (~100 lines)

**Optimization 1** (single-child And/Or):
```c
static int fold_single_child_intersect_union(query_step_t** steps) {
    query_step_t* s = *steps;
    query_step_t* prev = NULL;
    while (s) {
        if ((s->type == GRAPH_STEP_INTERSECT || s->type == GRAPH_STEP_UNION)
            && s->num_children == 1) {
            // Inline the single child chain after this step
            query_step_t* child = s->children[0];
            // Find tail of child chain
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
        }
        prev = s;
        s = s->next;
    }
    return 0;
}
```

## Tests

```
TEST(OptimizerTest, FoldSingleChildIntersect)
TEST(OptimizerTest, NoChangeForTwoChildIntersect)
TEST(OptimizerTest, MultipleSingleChildIntersects)
TEST(OptimizerTest, EmptyChainDoesNotCrash)
```