# Graph Query Optimizer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a query optimization pass that eliminates single-child Intersect/Union steps before execution.

**Architecture:** A single function `graph_optimize()` that walks the step chain and folds single-child INTERSECT/UNION steps by inlining their child chain. Called from `graph_query_execute_sync` before passing the chain to `graph_execute_chain`.

**Tech Stack:** C11, existing graph layer

---

## File Structure

```
src/Layers/graph/
├── graph_internal.h   — MODIFY: add graph_optimize() declaration
├── graph_optimizer.c  — CREATE: optimization pass
└── graph.c            — MODIFY: call graph_optimize() in graph_query_execute_sync

tests/test_graph_optimizer.cpp  — CREATE: optimizer tests
CMakeLists.txt                  — MODIFY: add source + test target
```

---

### Task 1: Declare + implement graph_optimize

**Files:**
- Create: `src/Layers/graph/graph_optimizer.c`
- Modify: `src/Layers/graph/graph_internal.h`

- [ ] **Step 1: Add declaration to graph_internal.h**

In `graph_internal.h`, add to the operator execution section:

```c
int graph_optimize(query_step_t** steps);
```

- [ ] **Step 2: Create graph_optimizer.c**

```c
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
```

- [ ] **Step 3: Commit**

```bash
git add src/Layers/graph/graph_internal.h src/Layers/graph/graph_optimizer.c
git commit -m "feat: add graph_optimize pass to fold single-child Intersect/Union"
```

---

### Task 2: Call optimizer from graph_query_execute_sync

**Files:**
- Modify: `src/Layers/graph/graph.c`

- [ ] **Step 1: Add optimizer call**

Find `graph_query_execute_sync` and add `graph_optimize(&q->head)` before `graph_execute_chain`:

```c
graph_result_t* graph_query_execute_sync(graph_query_t* q) {
    if (!q || !q->head) return NULL;

    graph_result_t* r = (graph_result_t*)get_clear_memory(sizeof(graph_result_t));
    vertex_set_init(&r->set, 64);

    graph_optimize(&q->head);

    int rc = graph_execute_chain(q->layer->db, q->head, &r->set);
    if (rc != 0) {
        vertex_set_destroy(&r->set);
        free(r);
        return NULL;
    }

    return r;
}
```

- [ ] **Step 2: Commit**

```bash
git add src/Layers/graph/graph.c
git commit -m "feat: call graph_optimize before execute chain"
```

---

### Task 3: Tests + CMake

**Files:**
- Create: `tests/test_graph_optimizer.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add source + test target to CMakeLists.txt**

Add to WAVEDB_SOURCES:
```cmake
    src/Layers/graph/graph_optimizer.c
```

Add test target:
```cmake
    # Test for Graph Optimizer
    add_executable(test_graph_optimizer tests/test_graph_optimizer.cpp)
    target_link_libraries(test_graph_optimizer wavedb gtest gtest_main)
    add_test(NAME test_graph_optimizer COMMAND test_graph_optimizer)
```

- [ ] **Step 2: Create tests/test_graph_optimizer.cpp**

```cpp
//
// Tests for Graph Query Optimizer
//

#include <gtest/gtest.h>
#include <string.h>

extern "C" {
#include "../src/Layers/graph/graph_internal.h"
}

class GraphOptimizerTest : public ::testing::Test {
protected:
    graph_layer_t* layer;
    void SetUp() override {
        layer = graph_layer_create(NULL, NULL);
        ASSERT_NE(layer, nullptr);
    }
    void TearDown() override {
        graph_layer_destroy(layer);
    }
};

TEST_F(GraphOptimizerTest, FoldSingleChildIntersect) {
    graph_query_t* q = graph_query_create(layer);
    graph_query_vertex(q, "clip_abc");

    // Create And(g.V("x").In("y")) — single child
    graph_query_t* child = graph_query_create(layer);
    graph_query_vertex(child, "gaming");
    graph_query_in(child, "tagged_with");
    graph_query_intersect(q, child, child);

    // q->head: VERTEX → INTERSECT[1 child: VERTEX → IN]
    ASSERT_NE(q->head, nullptr);
    ASSERT_EQ(q->head->type, GRAPH_STEP_VERTEX);
    ASSERT_NE(q->head->next, nullptr);
    ASSERT_EQ(q->head->next->type, GRAPH_STEP_INTERSECT);
    ASSERT_EQ(q->head->next->num_children, (size_t)1);

    graph_optimize(&q->head);

    // After: VERTEX → VERTEX → IN
    ASSERT_NE(q->head, nullptr);
    ASSERT_EQ(q->head->type, GRAPH_STEP_VERTEX);
    // Second step is the inlined child's VERTEX, not INTERSECT
    ASSERT_NE(q->head->next, nullptr);
    ASSERT_EQ(q->head->next->type, GRAPH_STEP_VERTEX);
    ASSERT_STREQ(q->head->next->vertex_id, "gaming");
    // Third step is the child's IN
    ASSERT_NE(q->head->next->next, nullptr);
    ASSERT_EQ(q->head->next->next->type, GRAPH_STEP_IN);

    graph_query_destroy(q);
    // Don't destroy child — its steps were consumed
}

TEST_F(GraphOptimizerTest, NoChangeForTwoChildIntersect) {
    graph_query_t* q = graph_query_create(layer);
    graph_query_vertex(q, "clip_abc");

    graph_query_t* left = graph_query_create(layer);
    graph_query_vertex(left, "gaming");
    graph_query_in(left, "tagged_with");

    graph_query_t* right = graph_query_create(layer);
    graph_query_vertex(right, "tutorial");
    graph_query_in(right, "tagged_with");

    graph_query_intersect(q, left, right);

    ASSERT_EQ(q->head->next->num_children, (size_t)2);
    graph_optimize(&q->head);
    // Should still be INTERSECT with 2 children
    ASSERT_EQ(q->head->next->type, GRAPH_STEP_INTERSECT);
    ASSERT_EQ(q->head->next->num_children, (size_t)2);

    graph_query_destroy(q);
}

TEST_F(GraphOptimizerTest, MultipleSingleChildIntersects) {
    // Build: V → AND(AND(V → IN)) → LIMIT
    graph_query_t* q = graph_query_create(layer);
    graph_query_vertex(q, "clip_abc");

    graph_query_t* inner = graph_query_create(layer);
    graph_query_vertex(inner, "gaming");
    graph_query_in(inner, "tagged_with");

    graph_query_t* outer = graph_query_create(layer);
    graph_query_vertex(outer, "x");
    graph_query_intersect(outer, inner, inner);

    graph_query_t* wrapper = graph_query_create(layer);
    graph_query_vertex(wrapper, "y");
    graph_query_intersect(wrapper, outer, outer);

    // Splice wrapper's chain into q
    // (just test with a simpler case: V → AND(AND(V → IN)))
    // Actually let me simplify — just test that a chain of single-child
    // intersects gets flattened
    graph_query_t* a1 = graph_query_create(layer);
    graph_query_vertex(a1, "gaming");
    graph_query_in(a1, "tagged_with");

    graph_query_t* a2 = graph_query_create(layer);
    graph_query_intersect(a2, a1, a1);

    graph_query_t* a3 = graph_query_create(layer);
    graph_query_intersect(a3, a2, a2);

    graph_query_t* main = graph_query_create(layer);
    graph_query_vertex(main, "clip_abc");
    graph_query_intersect(main, a3, a3);

    graph_optimize(&main->head);

    // After optimization: VERTEX → VERTEX → IN (3 steps, no INTERSECT)
    int steps = 0;
    query_step_t* s = main->head;
    while (s) { steps++; s = s->next; }
    ASSERT_EQ(steps, 3);
    // No INTERSECT steps survived
    s = main->head;
    while (s) { ASSERT_NE(s->type, GRAPH_STEP_INTERSECT); s = s->next; }

    graph_query_destroy(main);
}

TEST_F(GraphOptimizerTest, EmptyChainDoesNotCrash) {
    query_step_t* null_steps = NULL;
    int rc = graph_optimize(&null_steps);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(null_steps, nullptr);
}

TEST_F(GraphOptimizerTest, ExecutionStillWorks) {
    // Integration test: optimized query produces correct results
    graph_insert_sync(layer, "clip_abc", "tagged_with", "gaming");
    graph_insert_sync(layer, "clip_abc", "tagged_with", "tutorial");

    graph_query_t* child = graph_query_create(layer);
    graph_query_vertex(child, "clip_abc");
    graph_query_out(child, "tagged_with");

    graph_query_t* q = graph_query_create(layer);
    graph_query_vertex(q, "clip_abc");
    graph_query_intersect(q, child, child);

    graph_result_t* r = graph_query_execute_sync(q);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(graph_result_count(r), (size_t)2);

    const char* const* verts = graph_result_vertices(r);
    bool found_gaming = false, found_tutorial = false;
    for (size_t i = 0; i < graph_result_count(r); i++) {
        if (strcmp(verts[i], "gaming") == 0) found_gaming = true;
        if (strcmp(verts[i], "tutorial") == 0) found_tutorial = true;
    }
    EXPECT_TRUE(found_gaming);
    EXPECT_TRUE(found_tutorial);

    graph_result_destroy(r);
    graph_query_destroy(q);
}

TEST_F(GraphOptimizerTest, FoldSingleChildUnion) {
    graph_query_t* q = graph_query_create(layer);
    graph_query_vertex(q, "clip_abc");

    graph_query_t* child = graph_query_create(layer);
    graph_query_vertex(child, "gaming");
    graph_query_in(child, "tagged_with");
    graph_query_union(q, child, child);

    ASSERT_EQ(q->head->next->type, GRAPH_STEP_UNION);
    ASSERT_EQ(q->head->next->num_children, (size_t)1);

    graph_optimize(&q->head);

    // No UNION should remain
    query_step_t* s = q->head;
    while (s) { ASSERT_NE(s->type, GRAPH_STEP_UNION); s = s->next; }

    graph_query_destroy(q);
}
```

- [ ] **Step 3: Build and run**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build && cmake .. -DBUILD_TESTS=ON 2>&1 | tail -3 && make test_graph_optimizer 2>&1 | tail -10 && ./test_graph_optimizer 2>&1 | grep -E "PASSED|FAIL"
```

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt tests/test_graph_optimizer.cpp
git commit -m "feat: add graph optimizer tests and CMake integration"
```

---

### Task 4: Full Build + Verify

- [ ] **Step 1: Build and run ALL graph tests**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build && cmake .. -DBUILD_TESTS=ON 2>&1 | tail -2 && make test_graph test_graph_parser test_graph_set test_graph_schema test_graph_optimizer 2>&1 | tail -5 && ./test_graph && ./test_graph_parser && ./test_graph_set && ./test_graph_schema && ./test_graph_optimizer | grep -E "PASSED|FAIL|passed|failed"
```

Expected: all pass.

- [ ] **Step 2: Commit any final fixes**

```bash
git add -A && git commit -m "chore: finalize graph optimizer"
```