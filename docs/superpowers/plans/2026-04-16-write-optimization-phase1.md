# Write Optimization Phase 1: Quick Wins

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate debug logging overhead, make atomic refcounts the default, and combine hbtrie_node+bnode allocations for an estimated ~20% single-threaded write throughput improvement.

**Architecture:** Three independent optimizations that each pass all existing tests. Compile-out logging removes `log_debug`/`log_trace` calls from the binary at compile time. Atomic refcounts replace per-object `pthread_mutex_t` with C11 `_Atomic` operations. Combined pool allocation merges `hbtrie_node_t` and `bnode_t` into a single allocation.

**Tech Stack:** C11 (atomic primitives), existing memory pool, existing test suite (ctest)

**Spec:** `docs/superpowers/specs/2026-04-16-write-optimization-design.md` — Phase 1

---

## File Structure

| File | Responsibility |
|------|---------------|
| `src/Util/log.h` | Compile-time log level filtering macros |
| `src/RefCounter/refcounter.h` | Atomic-only `refcounter_t` struct and inline functions |
| `src/RefCounter/refcounter.c` | Atomic-only refcounter implementation |
| `src/HBTrie/hbtrie.h` | `hbtrie_combined_t` struct, `container_of` macro, updated `hbtrie_node_t` |
| `src/HBTrie/hbtrie.c` | `hbtrie_node_create`/`destroy` using combined allocation |
| `src/Util/memory_pool.h` | Add `container_of` macro |
| `src/Database/database_config.h` | No changes needed |

---

### Task 1: Compile-Out Debug Logging

**Files:**
- Modify: `src/Util/log.h`

- [ ] **Step 1: Add compile-time level macros to log.h**

Open `src/Util/log.h`. Above the existing `log_trace`/`log_debug`/etc macros (around line 31), add compile-time filtering:

Replace the existing enum and macro block (lines 31-38):
```c
enum { LOG_TRACE, LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR, LOG_FATAL };

#define log_trace(...) log_log(LOG_TRACE, __FILE__, __LINE__, __VA_ARGS__)
#define log_debug(...) log_log(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define log_info(...)  log_log(LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define log_warn(...)  log_log(LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define log_error(...) log_log(LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define log_fatal(...) log_log(LOG_FATAL, __FILE__, __LINE__, __VA_ARGS__)
```

With:
```c
enum { LOG_LEVEL_TRACE = 0, LOG_LEVEL_DEBUG = 1, LOG_LEVEL_INFO = 2,
       LOG_LEVEL_WARN = 3, LOG_LEVEL_ERROR = 4, LOG_LEVEL_FATAL = 5 };

/* Legacy aliases for enum values used in log_log() calls */
#define LOG_TRACE  LOG_LEVEL_TRACE
#define LOG_DEBUG  LOG_LEVEL_DEBUG
#define LOG_INFO   LOG_LEVEL_INFO
#define LOG_WARN   LOG_LEVEL_WARN
#define LOG_ERROR  LOG_LEVEL_ERROR
#define LOG_FATAL  LOG_LEVEL_FATAL

#ifndef LOG_COMPILE_LEVEL
#define LOG_COMPILE_LEVEL LOG_LEVEL_INFO
#endif

#if LOG_COMPILE_LEVEL <= LOG_LEVEL_TRACE
#define log_trace(...) log_log(LOG_TRACE, __FILE__, __LINE__, __VA_ARGS__)
#else
#define log_trace(...) ((void)0)
#endif

#if LOG_COMPILE_LEVEL <= LOG_LEVEL_DEBUG
#define log_debug(...) log_log(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#else
#define log_debug(...) ((void)0)
#endif

#if LOG_COMPILE_LEVEL <= LOG_LEVEL_INFO
#define log_info(...)  log_log(LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#else
#define log_info(...)  ((void)0)
#endif

/* warn, error, fatal are always compiled in — they're rare and important */
#define log_warn(...)  log_log(LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define log_error(...) log_log(LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define log_fatal(...) log_log(LOG_FATAL, __FILE__, __LINE__, __VA_ARGS__)
```

- [ ] **Step 2: Build and run tests**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build && cmake -DCMAKE_BUILD_TYPE=Release .. && make -j$(nproc) && ctest --output-on-failure`

Expected: All tests pass. Debug/trace log calls are compiled out with the default `LOG_COMPILE_LEVEL=LOG_LEVEL_INFO`.

- [ ] **Step 3: Verify debug calls are eliminated**

Run: `nm build/src/WaveDB/libwavedb.a | grep log_log | wc -l`

Then rebuild with `-DLOG_COMPILE_LEVEL=0` (all logging enabled):
Run: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build && cmake -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-DLOG_COMPILE_LEVEL=0" .. && make -j$(nproc)`

Verify debug builds still produce log output.

- [ ] **Step 4: Run valgrind leak check**

Run: `valgrind --leak-check=full --error-exitcode=1 ./build/tests/test_database 2>&1 | tail -20`

Expected: Zero leaks.

- [ ] **Step 5: Run sync benchmark for baseline comparison**

Run: `./build/tests/benchmark/benchmark_database_sync 2>&1`

Record put/get/delete/mixed throughput numbers for comparison after Phase 1 is complete.

- [ ] **Step 6: Commit**

```bash
git add src/Util/log.h
git commit -m "feat: add compile-time log level filtering

Default LOG_COMPILE_LEVEL=LOG_LEVEL_INFO eliminates log_debug and
log_trace calls from the binary. Override with -DLOG_COMPILE_LEVEL=0
for debug builds. ~7% single-threaded write improvement expected."
```

---

### Task 2: Make REFCOUNTER_ATOMIC the Default

**Files:**
- Modify: `src/RefCounter/refcounter.h`
- Modify: `src/RefCounter/refcounter.c`

- [ ] **Step 1: Rewrite refcounter.h to atomic-only**

Open `src/RefCounter/refcounter.h`. Replace the entire `refcounter_t` struct and function declarations (lines 21-38) with atomic-only versions:

```c
typedef struct refcounter_t {
    _Atomic uint_fast16_t count;
    _Atomic uint_fast8_t yield;
} refcounter_t;

void refcounter_init(refcounter_t* refcounter);
void refcounter_yield(refcounter_t* refcounter);
void* refcounter_reference(refcounter_t* refcounter);
void refcounter_dereference(refcounter_t* refcounter);
refcounter_t* refcounter_consume(refcounter_t** refcounter);
uint16_t refcounter_count(refcounter_t* refcounter);
```

Remove `refcounter_destroy_lock` — it's a no-op in atomic mode and no longer needed.

Keep the macros unchanged:
```c
#define REFERENCE(N,T) (T*) refcounter_reference((refcounter_t*) N)
#define YIELD(N) refcounter_yield((refcounter_t*) N)
#define DEREFERENCE(N) refcounter_dereference((refcounter_t*) N); N = NULL
#define DESTROY(N,T)  T##_destroy(N); N = NULL
#define CONSUME(N, T) (T*) refcounter_consume((refcounter_t**) &N)
```

Remove all `#ifdef REFCOUNTER_ATOMIC` / `#else` conditionals — keep only the atomic code paths.

- [ ] **Step 2: Rewrite refcounter.c to atomic-only**

Open `src/RefCounter/refcounter.c`. Replace all function implementations with atomic-only versions:

```c
#include "refcounter.h"
#include <stdlib.h>
#include <stdatomic.h>

void refcounter_init(refcounter_t* refcounter) {
    if (refcounter == NULL) return;
    atomic_store(&refcounter->count, 1);
    atomic_store(&refcounter->yield, 0);
}

void refcounter_yield(refcounter_t* refcounter) {
    if (refcounter == NULL) return;
    atomic_fetch_add(&refcounter->yield, 1);
}

void* refcounter_reference(refcounter_t* refcounter) {
    if (refcounter == NULL) return NULL;

    // Try to consume a yield first (ownership transfer)
    uint8_t expected = atomic_load(&refcounter->yield);
    while (expected > 0) {
        if (atomic_compare_exchange_weak(&refcounter->yield, &expected, expected - 1)) {
            return (void*) refcounter;
        }
    }

    // No yield available, increment count
    atomic_fetch_add(&refcounter->count, 1);
    return (void*) refcounter;
}

void refcounter_dereference(refcounter_t* refcounter) {
    if (refcounter == NULL) return;

    // Try to consume a yield first
    uint8_t expected = atomic_load(&refcounter->yield);
    while (expected > 0) {
        if (atomic_compare_exchange_weak(&refcounter->yield, &expected, expected - 1)) {
            return;
        }
    }

    // No yield available, decrement count
    atomic_fetch_sub(&refcounter->count, 1);
}

refcounter_t* refcounter_consume(refcounter_t** refcounter) {
    if (refcounter == NULL || *refcounter == NULL) return NULL;
    refcounter_yield(*refcounter);
    refcounter_t* holder = *refcounter;
    *refcounter = NULL;
    return holder;
}

uint16_t refcounter_count(refcounter_t* refcounter) {
    if (refcounter == NULL) return 0;
    return atomic_load(&refcounter->count);
}
```

Remove `refcounter_destroy_lock` entirely — it's no longer needed.

- [ ] **Step 3: Remove any `refcounter_destroy_lock` calls from the codebase**

Search for all calls to `refcounter_destroy_lock` and remove them. These are no-ops in atomic mode and the function no longer exists.

Run: `grep -rn "refcounter_destroy_lock" src/`

Remove each call site found.

- [ ] **Step 4: Remove `REFCOUNTER_ATOMIC` ifdef from build system**

Search for any CMake or source references to `REFCOUNTER_ATOMIC`:

Run: `grep -rn "REFCOUNTER_ATOMIC" src/ CMakeLists.txt`

Remove any `#ifdef REFCOUNTER_ATOMIC` / `#endif` blocks, keeping only the atomic code paths. Also remove any CMake definitions like `add_definitions(-DREFCOUNTER_ATOMIC)`.

- [ ] **Step 5: Build and run tests**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build && cmake -DCMAKE_BUILD_TYPE=Release .. && make -j$(nproc) && ctest --output-on-failure`

Expected: All tests pass. The atomic refcounter has identical semantics to the old `REFCOUNTER_ATOMIC` path.

- [ ] **Step 6: Run valgrind leak check**

Run: `valgrind --leak-check=full --error-exitcode=1 ./build/tests/test_database 2>&1 | tail -20`

Expected: Zero leaks. Pay special attention to reference count correctness — verify no double-frees or leaks in hbtrie node lifecycle.

- [ ] **Step 7: Run valgrind on all test suites**

Run: `valgrind --leak-check=full ./build/tests/test_hbtrie 2>&1 | tail -5`
Run: `valgrind --leak-check=full ./build/tests/test_bnode 2>&1 | tail -5`

Expected: Zero leaks in all test suites.

- [ ] **Step 8: Commit**

```bash
git add src/RefCounter/refcounter.h src/RefCounter/refcounter.c
git commit -m "feat: make REFCOUNTER_ATOMIC the default, remove non-atomic path

Remove #ifdef REFCOUNTER_ATOMIC conditional compilation. The atomic
implementation (C11 _Atomic) is now the only implementation. Removes
per-object pthread_mutex_t (40 bytes) from refcounter_t, shrinking it
to 4 bytes. ~13% single-threaded write improvement expected."
```

---

### Task 3: Combined Pool Allocation for hbtrie_node + bnode

**Files:**
- Modify: `src/Util/memory_pool.h` (add `container_of` macro)
- Modify: `src/HBTrie/hbtrie.h` (add `hbtrie_combined_t`, update `hbtrie_node_t`)
- Modify: `src/HBTrie/hbtrie.c` (update `hbtrie_node_create`/`destroy`)

- [ ] **Step 1: Add `container_of` macro to memory_pool.h**

Open `src/Util/memory_pool.h`. Add near the top (after includes, before the struct definitions):

```c
#ifndef container_of
#define container_of(ptr, type, member) \
    ((type*)((char*)(ptr) - offsetof(type, member)))
#endif
```

- [ ] **Step 2: Add `hbtrie_combined_t` struct and update `hbtrie_node_t` in hbtrie.h**

Open `src/HBTrie/hbtrie.h`. After the `bnode_t` forward declaration and before the `hbtrie_node_t` struct definition, add:

```c
typedef struct hbtrie_combined_t {
    hbtrie_node_t node;
    bnode_t bnode;
} hbtrie_combined_t;
```

In the `hbtrie_node_t` struct (lines 42-54), the `bnode_t* btree` field stays as-is — it points to the combined allocation's `bnode` member. No struct changes needed.

- [ ] **Step 3: Update `hbtrie_node_create` to use combined allocation**

Open `src/HBTrie/hbtrie.c`. Find `hbtrie_node_create` (around line 411). Replace:

```c
hbtrie_node_t* hbtrie_node_create(uint32_t btree_node_size) {
  hbtrie_node_t* node = memory_pool_alloc(sizeof(hbtrie_node_t));

  node->btree = bnode_create(btree_node_size);
  if (node->btree == NULL) {
    memory_pool_free(node, sizeof(hbtrie_node_t));
    return NULL;
  }
  // ... rest of init ...
```

With:

```c
hbtrie_node_t* hbtrie_node_create(uint32_t btree_node_size) {
  hbtrie_combined_t* combined = memory_pool_alloc(sizeof(hbtrie_combined_t));
  if (combined == NULL) return NULL;

  hbtrie_node_t* node = &combined->node;
  bnode_t* btree = &combined->bnode;

  // Initialize bnode in-place
  bnode_init(btree, btree_node_size);

  node->btree = btree;
  node->btree_height = 1;  // Single leaf bnode

  // Initialize page file storage tracking (in-memory by default)
  node->disk_offset = (uint64_t)-1;  // UINT64_MAX = not yet persisted
  node->is_loaded = 1;               // Newly created nodes are in memory
  node->is_dirty = 0;                // Not modified yet

  atomic_init(&node->seq, 0);
  spinlock_init(&node->write_lock);
  refcounter_init((refcounter_t*)node);

  return node;
}
```

Note: `spinlock_init` is used here because Task 3 may be implemented before or after the spinlock task. If spinlocks haven't been implemented yet, use `platform_lock_init(&node->write_lock)` instead. The plan assumes Phase 1 Task 3 may be implemented independently.

**Wait** — since this task is in Phase 1 and spinlocks are Phase 3, use `platform_lock_init` here:

```c
  platform_lock_init(&node->write_lock);
```

- [ ] **Step 4: Update `hbtrie_node_destroy` to free combined allocation**

Find `hbtrie_node_destroy` in `src/HBTrie/hbtrie.c`. In the section where it frees the node, change from freeing `hbtrie_node_t` individually to freeing the combined allocation:

Find the line that frees the node (should be near the end of the destroy function):
```c
  memory_pool_free(node, sizeof(hbtrie_node_t));
```

Replace with:
```c
  hbtrie_combined_t* combined = container_of(node, hbtrie_combined_t, node);
  memory_pool_free(combined, sizeof(hbtrie_combined_t));
```

Also find and remove the line that frees `btree` separately (if it exists — in the current code, `bnode_destroy` frees the bnode, but with combined allocation, the bnode is part of the combined struct and should NOT be freed separately):

Find: `bnode_destroy(node->btree);` or similar bnode cleanup.
Replace with: Remove the `bnode_destroy` call — the bnode is part of the combined allocation and will be freed when `memory_pool_free(combined, sizeof(hbtrie_combined_t))` is called.

Instead, call `bnode_deinit(node->btree)` to clean up bnode internals (free its entries vector) without freeing the bnode struct itself. If `bnode_deinit` doesn't exist, add it as a companion to `bnode_create` that frees the entries vector and any internal allocations without freeing the `bnode_t` struct itself.

- [ ] **Step 5: Add `bnode_deinit` if it doesn't exist**

Check if `bnode_deinit` exists:

Run: `grep -n "bnode_deinit" src/HBTrie/bnode.h src/HBTrie/bnode.c`

If it doesn't exist, add it to `src/HBTrie/bnode.h`:
```c
void bnode_deinit(bnode_t* node);
```

And to `src/HBTrie/bnode.c`:
```c
void bnode_deinit(bnode_t* node) {
    if (node == NULL) return;
    // Free entries vector and internal allocations, but NOT the bnode_t struct itself
    for (int i = 0; i < node->entries.length; i++) {
        bnode_entry_t* entry = &node->entries.data[i];
        if (entry->has_value && entry->value != NULL) {
            identifier_destroy(entry->value);
            entry->value = NULL;
        }
        if (entry->has_value && entry->versions != NULL) {
            version_entry_t* current = entry->versions;
            while (current != NULL) {
                version_entry_t* next = current->next;
                if (current->value != NULL) {
                    identifier_destroy(current->value);
                }
                free(current);
                current = next;
            }
            entry->versions = NULL;
        }
        chunk_t* key = bnode_entry_get_key(entry);
        if (key != NULL) {
            chunk_destroy(key);
        }
    }
    vec_deinit(&node->entries);
}
```

Also add `bnode_init` if it doesn't exist (for initializing a bnode in-place without allocation):

```c
void bnode_init(bnode_t* node, uint32_t btree_node_size) {
    if (node == NULL) return;
    memset(node, 0, sizeof(bnode_t));
    refcounter_init((refcounter_t*)node);
    node->level = 1;
    vec_init(&node->entries, sizeof(bnode_entry_t), 4);
    node->max_size = btree_node_size;
}
```

- [ ] **Step 6: Build and run tests**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build && cmake -DCMAKE_BUILD_TYPE=Release .. && make -j$(nproc) && ctest --output-on-failure`

Expected: All tests pass. `hbtrie_node_create` now allocates `hbtrie_combined_t` as a single block.

- [ ] **Step 7: Run valgrind leak check**

Run: `valgrind --leak-check=full --error-exitcode=1 ./build/tests/test_hbtrie 2>&1 | tail -20`
Run: `valgrind --leak-check=full --error-exitcode=1 ./build/tests/test_bnode 2>&1 | tail -20`

Expected: Zero leaks. The combined allocation must be freed exactly once — no double-frees from destroying the bnode separately.

- [ ] **Step 8: Commit**

```bash
git add src/Util/memory_pool.h src/HBTrie/hbtrie.h src/HBTrie/hbtrie.c src/HBTrie/bnode.h src/HBTrie/bnode.c
git commit -m "feat: combine hbtrie_node and bnode into single pool allocation

Allocate hbtrie_node_t and bnode_t as a single hbtrie_combined_t
block, reducing 2 allocations to 1 and improving cache locality.
Use container_of macro to find the combined allocation from the node
pointer. Add bnode_init/bnode_deinit for in-place init/cleanup.
~5% improvement expected."
```

---

### Task 4: Phase 1 Benchmark Comparison

- [ ] **Step 1: Run sync benchmark**

Run: `./build/tests/benchmark/benchmark_database_sync 2>&1`

Record put/get/delete/mixed throughput numbers. Compare with baseline from Task 1 Step 5.

Expected: Put throughput should be ~20% higher than baseline (from ~63K to ~80K ops/sec).

- [ ] **Step 2: Run concurrent benchmark**

Run: `./build/tests/benchmark/benchmark_database 2>&1`

Record write/read/mixed throughput at 4, 8, 16, 32 threads.

- [ ] **Step 3: Record results**

Update `.benchmarks/` JSON files with new numbers if they represent an improvement.

- [ ] **Step 4: Commit benchmark results**

```bash
git add .benchmarks/
git commit -m "bench: update benchmark results after Phase 1 optimizations"
```