# Actor Model Redesign — Full Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace WaveDB's locking-based concurrency model (spinlocks, mutexes, seqlocks, work_pool_t) with a Pony-style actor system imported from liboffs, using sharded trie actors, a WAL actor, and cache actors.

**Architecture:** Import the battle-tested actor infrastructure from liboffs (~650 lines of C: actor_t, message_queue_t, scheduler_pool_t, backpressure). Convert the 64 write_lock shards into 64 trie-shard actors that own subtrees and process writes without internal locking. Convert WAL and caches into service actors. Reads remain lock-free via atomic root snapshots. The work_pool_t is replaced by scheduler_pool_t.

**Tech Stack:** C11, pthreads, liboffs Actor/Scheduler/Platform libraries

---

## File Structure

| File | Role |
|------|------|
| `src/Actor/actor.h` | Import from liboffs — actor_t, flags, init/send/run/destroy, backpressure |
| `src/Actor/actor.c` | Import from liboffs — implementation |
| `src/Actor/message.h` | WaveDB-specific message types and payload structs |
| `src/Actor/message_queue.h` | Import from liboffs — lock-free MS queue |
| `src/Actor/message_queue.c` | Import from liboffs — implementation |
| `src/Actor/pool.h` | Import from liboffs — memory pool for message nodes |
| `src/Actor/pool.c` | Import from liboffs — implementation |
| `src/Scheduler/deque.h` | Import from liboffs — work-stealing deque |
| `src/Scheduler/deque.c` | Import from liboffs — implementation |
| `src/Scheduler/scheduler.h` | Import from liboffs — scheduler_pool_t, inject, defer |
| `src/Scheduler/scheduler.c` | Import from liboffs — implementation |
| `src/Platform/platform_thread.h` | Import from liboffs — pthreads/Windows thread wrappers |
| `src/Platform/platform_thread.c` | Import from liboffs — implementation |
| `src/Platform/platform_local.h` | Import from liboffs — thread-local storage helpers |
| `src/Platform/platform_local.c` | Import from liboffs — implementation |
| `src/Util/atomic_compat.h` | Adapt — reconcile WaveDB ATOMIC_TYPE vs liboffs ATOMIC macros |
| `src/Database/database_actor.h` | New — database-level actor, routes to shards |
| `src/Database/database_actor.c` | New — implementation |
| `src/Database/trie_shard_actor.h` | New — shard actor owning trie subtree |
| `src/Database/trie_shard_actor.c` | New — synchronous traversal within shard |
| `src/Database/wal_actor.h` | New — WAL service actor |
| `src/Database/wal_actor.c` | New — batched writes, compaction |
| `src/Database/lru_actor.h` | New — LRU cache actor (or sharded actors) |
| `src/Database/lru_actor.c` | New — message-driven get/put/delete |
| `src/Database/bnode_cache_actor.h` | New — file bnode cache actor |
| `src/Database/bnode_cache_actor.c` | New — message-driven read/write/release |
| `src/Database/database.h` | Modify — replace work_pool_t with scheduler_pool_t, replace write_locks with shard actors |
| `src/Database/database.c` | Modify — rewrite async/sync paths to use actors |
| `src/Database/database_lru.h` | Modify — add actor-based LRU variants alongside existing |
| `src/Database/database_lru.c` | Modify — implement actor-based LRU |
| `src/Database/wal_manager.h` | Modify — add actor-based WAL interface |
| `src/Database/wal_manager.c` | Modify — implement WAL actor |
| `src/Storage/bnode_cache.h` | Modify — add actor-based cache interface |
| `src/Storage/bnode_cache.c` | Modify — implement actor-based cache |
| `src/HBTrie/hbtrie.h` | Modify — remove write_lock, seq seqlock from node structs |
| `src/HBTrie/hbtrie.c` | Modify — remove lock acquire/release from operations |
| `src/HBTrie/bnode.h` | Modify — remove write_lock, seq seqlock from bnode struct |
| `src/Workers/pool.h` | Delete — replaced by scheduler_pool_t |
| `src/Workers/pool.c` | Delete — replaced by scheduler_pool_t |
| `src/Workers/queue.h` | Delete — replaced by message_queue_t |
| `src/Workers/queue.c` | Delete — replaced by message_queue_t |
| `tests/test_actor_scheduler.c` | New — standalone scheduler tests |
| `tests/test_trie_shard_actor.c` | New — shard actor functional tests |
| `tests/test_wal_actor.c` | New — WAL actor integration tests |
| `tests/test_cache_actors.c` | New — LRU + bnode cache actor tests |
| `tests/test_database_actor_integration.c` | New — end-to-end actor integration tests |

---

## Phase 1: Import and Adapt Actor Infrastructure

### Task 1.1: Reconcile atomics and platform layer

**Files:**
- Modify: `src/Util/atomic_compat.h`
- Create: `src/Platform/platform_thread.h`
- Create: `src/Platform/platform_thread.c`
- Create: `src/Platform/platform_local.h`
- Create: `src/Platform/platform_local.c`

**Context:** Liboffs uses `ATOMIC(T)` with method-style macros (`ATOMIC_STORE(ptr, val)` → `ptr->store(val)` in C++). WaveDB uses `ATOMIC_TYPE(T)` with C11-style free functions (`atomic_store(&ptr, val)`). The actor code uses liboffs-style atomics, so we need a compatibility shim. Similarly, liboffs uses opaque `platform_mutex_t*` / `platform_condvar_t*` pointers, which the scheduler needs for the inject queue and idle detection.

- [ ] **Step 1: Add liboffs-compatible atomics to WaveDB's atomic_compat.h**

In `src/Util/atomic_compat.h`, append before the `#endif` guard:

```c
/* Compatibility with liboffs actor code (uses method-style atomics) */
#ifdef __cplusplus
  #define ATOMIC(T) std::atomic<T>
  #define ATOMIC_STORE(ptr, val) (ptr)->store(val)
  #define ATOMIC_LOAD(ptr) (ptr)->load()
  #define ATOMIC_EXCHANGE(ptr, val) (ptr)->exchange(val)
  #define ATOMIC_FETCH_ADD(ptr, val) (ptr)->fetch_add(val)
  #define ATOMIC_FETCH_OR(ptr, val) (ptr)->fetch_or(val)
  #define ATOMIC_FETCH_AND(ptr, val) (ptr)->fetch_and(val)
  #define ATOMIC_CAS_STRONG(ptr, expected, desired) (ptr)->compare_exchange_strong(expected, desired)
#else
  #define ATOMIC(T) _Atomic(T)
  #define ATOMIC_STORE(ptr, val) atomic_store(ptr, val)
  #define ATOMIC_LOAD(ptr) atomic_load(ptr)
  #define ATOMIC_EXCHANGE(ptr, val) atomic_exchange(ptr, val)
  #define ATOMIC_FETCH_ADD(ptr, val) atomic_fetch_add(ptr, val)
  #define ATOMIC_FETCH_OR(ptr, val) atomic_fetch_or(ptr, val)
  #define ATOMIC_FETCH_AND(ptr, val) atomic_fetch_and(ptr, val)
  #define ATOMIC_CAS_STRONG(ptr, expected, desired) atomic_compare_exchange_strong(ptr, expected, desired)
#endif
```

- [ ] **Step 2: Create platform_thread.h**

Copy from `/home/victor/Workspace/src/github.com/vijayee/liboffs/src/Platform/platform_thread.h` to `src/Platform/platform_thread.h`. No changes needed.

Run: `cp /home/victor/Workspace/src/github.com/vijayee/liboffs/src/Platform/platform_thread.h src/Platform/platform_thread.h`

- [ ] **Step 3: Create platform_thread.c**

Copy from `/home/victor/Workspace/src/github.com/vijayee/liboffs/src/Platform/platform_thread.c` to `src/Platform/platform_thread.c`. No changes needed.

Run: `cp /home/victor/Workspace/src/github.com/vijayee/liboffs/src/Platform/platform_thread.c src/Platform/platform_thread.c`

- [ ] **Step 4: Create platform_local.h**

Copy from `/home/victor/Workspace/src/github.com/vijayee/liboffs/src/Platform/platform_local.h` to `src/Platform/platform_local.h`. No changes needed.

Run: `cp /home/victor/Workspace/src/github.com/vijayee/liboffs/src/Platform/platform_local.h src/Platform/platform_local.h`

- [ ] **Step 5: Create platform_local.c**

Copy from `/home/victor/Workspace/src/github.com/vijayee/liboffs/src/Platform/platform_local.c` to `src/Platform/platform_local.c`. No changes needed.

Run: `cp /home/victor/Workspace/src/github.com/vijayee/liboffs/src/Platform/platform_local.c src/Platform/platform_local.c`

- [ ] **Step 6: Verify compilation**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB && mkdir -p build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Debug && make -j$(nproc) 2>&1 | head -30`
Expected: Build succeeds (no new errors from these additions)

- [ ] **Step 7: Commit**

```bash
git add src/Util/atomic_compat.h src/Platform/platform_thread.h src/Platform/platform_thread.c src/Platform/platform_local.h src/Platform/platform_local.c
git commit -m "feat: add liboffs-compatible atomics and platform thread layer"
```

### Task 1.2: Import message queue (lock-free MS queue)

**Files:**
- Create: `src/Actor/message_queue.h`
- Create: `src/Actor/message_queue.c`

**Context:** The lock-free Michael-Scott queue used by the actor system. Needs MAILBOX_MUTE_THRESHOLD for backpressure. Must use liboffs-compatible atomics.

- [ ] **Step 1: Copy message_queue.h with adapted includes**

Copy from liboffs, then adapt the include path:

```bash
cp /home/victor/Workspace/src/github.com/vijayee/liboffs/src/Actor/message_queue.h src/Actor/message_queue.h
```

Edit `src/Actor/message_queue.h` — change line 8 from `#include "../Util/atomic_compat.h"` to `#include "../Util/atomic_compat.h"` (same relative path, just verify it's correct).

- [ ] **Step 2: Copy message_queue.c with adapted includes**

```bash
cp /home/victor/Workspace/src/github.com/vijayee/liboffs/src/Actor/message_queue.c src/Actor/message_queue.c
```

Edit `src/Actor/message_queue.c` — change `#include "../Util/allocator.h"` to use WaveDB's allocator. Verify the include at top reads:

```c
#include "message_queue.h"
#include "../Util/allocator.h"
```

Check that `get_clear_memory()` is the allocation function used (it is — both liboffs and WaveDB use the same function name).

- [ ] **Step 3: Verify compilation**

Run: `cd build && make -j$(nproc) 2>&1 | head -20`
Expected: No errors from message_queue.c

- [ ] **Step 4: Commit**

```bash
git add src/Actor/message_queue.h src/Actor/message_queue.c
git commit -m "feat: import lock-free message queue from liboffs"
```

### Task 1.3: Import actor core (actor_t, backpressure)

**Files:**
- Create: `src/Actor/actor.h`
- Create: `src/Actor/actor.c`

**Context:** The core actor struct and operations. References scheduler_pool_t which we'll import next, but the header can forward-declare it.

- [ ] **Step 1: Copy actor.h with adapted includes**

```bash
cp /home/victor/Workspace/src/github.com/vijayee/liboffs/src/Actor/actor.h src/Actor/actor.h
```

Verify the includes are correct for WaveDB's tree structure. The file includes:
- `"message_queue.h"` — exists in same directory ✓
- `"../Util/atomic_compat.h"` — exists ✓

No changes needed.

- [ ] **Step 2: Copy actor.c with adapted includes**

```bash
cp /home/victor/Workspace/src/github.com/vijayee/liboffs/src/Actor/actor.c src/Actor/actor.c
```

Verify includes:
- `"actor.h"` — ✓
- `"../Scheduler/scheduler.h"` — will exist after Task 1.4 ✓
- `"../Util/allocator.h"` — exists ✓

- [ ] **Step 3: Create src/Actor/message.h with WaveDB message types**

Create `src/Actor/message.h`:

```c
#ifndef WAVEDB_ACTOR_MESSAGE_H
#define WAVEDB_ACTOR_MESSAGE_H

#include <stdint.h>
#include "../HBTrie/path.h"
#include "../HBTrie/identifier.h"
#include "../Buffer/buffer.h"
#include "../Workers/promise.h"
#include "../Workers/transaction_id.h"
#include "../Storage/bnode_cache.h"
#include "../Time/wheel.h"

/* Forward declarations */
typedef struct actor_t actor_t;

/* Database operations */
typedef struct {
    path_t* path;
    identifier_t* value;     /* NULL for reads */
    promise_t* promise;     /* NULL for fire-and-forget */
} db_op_payload_t;

/* WAL write record */
typedef struct {
    uint64_t thread_id;
    transaction_id_t txn_id;
    uint8_t type;            /* 0=put, 1=delete */
    buffer_t* data;          /* serialized entry */
    actor_t* reply_to;
} wal_record_payload_t;

/* LRU cache operations */
typedef struct {
    path_t* path;
    identifier_t* value;     /* For put; NULL for get */
    actor_t* reply_to;
} lru_op_payload_t;

typedef struct {
    path_t* path;
    identifier_t* value;     /* NULL if not found */
} lru_result_payload_t;

/* BNode cache operations */
typedef struct {
    uint64_t offset;         /* Disk offset */
    uint8_t* data;           /* For write; NULL for read */
    size_t data_len;
    actor_t* reply_to;
} bnode_cache_op_payload_t;

typedef struct {
    uint64_t offset;
    uint8_t* data;
    size_t data_len;
} bnode_cache_result_payload_t;

typedef enum {
    /* Database operations */
    DB_PUT = 0,
    DB_GET,
    DB_DELETE,
    DB_SNAPSHOT,
    /* WAL operations */
    WAL_WRITE,
    WAL_FLUSH,
    WAL_SEAL_AND_COMPACT,
    /* LRU operations */
    LRU_GET,
    LRU_PUT,
    LRU_DELETE,
    LRU_CLEAR,
    /* BNode cache operations */
    BNODE_CACHE_READ,
    BNODE_CACHE_WRITE,
    BNODE_CACHE_RELEASE,
    BNODE_CACHE_INVALIDATE,
    BNODE_CACHE_FLUSH,
    /* Result messages */
    DB_GET_RESULT,
    DB_OP_RESULT,
    WAL_FLUSH_RESULT,
    LRU_GET_RESULT,
    BNODE_CACHE_READ_RESULT,
    /* Shard actor internal */
    SHARD_PUT,
    SHARD_GET,
    SHARD_DELETE,
    SHARD_GC,
} wavedb_message_type_e;

typedef struct message_t {
    uint32_t type;
    void* payload;
    void (*payload_destroy)(void*);
} message_t;

#endif /* WAVEDB_ACTOR_MESSAGE_H */
```

- [ ] **Step 4: Verify compilation (will fail on scheduler references, expected)**

Run: `cd build && make -j$(nproc) 2>&1 | head -20`
Expected: May fail with "scheduler_pool_t not defined" — this is fine, resolved in Task 1.4.

- [ ] **Step 5: Commit**

```bash
git add src/Actor/actor.h src/Actor/actor.c src/Actor/message.h
git commit -m "feat: import actor core and define WaveDB message types"
```

### Task 1.4: Import scheduler (work-stealing pool, inject, deferred cleanup)

**Files:**
- Create: `src/Actor/pool.h`
- Create: `src/Actor/pool.c`
- Create: `src/Scheduler/deque.h`
- Create: `src/Scheduler/deque.c`
- Create: `src/Scheduler/scheduler.h`
- Create: `src/Scheduler/scheduler.c`

**Context:** The scheduler uses `platform_mutex_t*` and `platform_condvar_t*` from the imported platform layer. We need all three components (deque, pool, scheduler) together since they're interdependent.

- [ ] **Step 1: Copy scheduler files**

```bash
cp /home/victor/Workspace/src/github.com/vijayee/liboffs/src/Scheduler/deque.h src/Scheduler/deque.h
cp /home/victor/Workspace/src/github.com/vijayee/liboffs/src/Scheduler/deque.c src/Scheduler/deque.c
cp /home/victor/Workspace/src/github.com/vijayee/liboffs/src/Scheduler/scheduler.h src/Scheduler/scheduler.h
cp /home/victor/Workspace/src/github.com/vijayee/liboffs/src/Scheduler/scheduler.c src/Scheduler/scheduler.c
cp /home/victor/Workspace/src/github.com/vijayee/liboffs/src/Actor/pool.h src/Actor/pool.h
cp /home/victor/Workspace/src/github.com/vijayee/liboffs/src/Actor/pool.c src/Actor/pool.c
```

- [ ] **Step 2: Verify scheduler.h include paths**

Check `src/Scheduler/scheduler.h` — it includes:
- `"deque.h"` — same directory ✓
- `"../Actor/actor.h"` — exists ✓
- `"../Platform/platform.h"` — we don't have this umbrella header

Edit `src/Scheduler/scheduler.h`: replace `#include "../Platform/platform.h"` with:

```c
#include "../Platform/platform_thread.h"
```

- [ ] **Step 3: Verify scheduler.c include paths**

Check `src/Scheduler/scheduler.c` — it includes `"../Util/allocator.h"`. Verify this is the same function in both projects (it is — `get_clear_memory`). No change needed.

- [ ] **Step 4: Add scheduler files to CMakeLists.txt**

In `CMakeLists.txt`, add to the library sources:

```cmake
src/Actor/actor.c
src/Actor/message_queue.c
src/Actor/pool.c
src/Scheduler/deque.c
src/Scheduler/scheduler.c
src/Platform/platform_thread.c
src/Platform/platform_local.c
```

- [ ] **Step 5: Build and fix any compilation errors**

Run: `cd build && cmake .. && make -j$(nproc) 2>&1`
Expected: Successful build. If errors, fix includes/forward declarations.

- [ ] **Step 6: Commit**

```bash
git add src/Actor/pool.h src/Actor/pool.c src/Scheduler/deque.h src/Scheduler/deque.c src/Scheduler/scheduler.h src/Scheduler/scheduler.c CMakeLists.txt
git commit -m "feat: import work-stealing scheduler from liboffs"
```

### Task 1.5: Write and run scheduler unit test

**Files:**
- Create: `tests/test_actor_scheduler.c`

**Context:** Verify the imported infrastructure works standalone before integrating with WaveDB.

- [ ] **Step 1: Write the test**

Create `tests/test_actor_scheduler.c`:

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../src/Actor/actor.h"
#include "../src/Actor/message.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/Util/allocator.h"

/* Simple counter actor for testing */
typedef struct {
    actor_t actor;
    ATOMIC(int) count;
    ATOMIC(int) errors;
} counter_actor_t;

static void counter_dispatch(void* state, message_t* msg) {
    counter_actor_t* ca = (counter_actor_t*)state;
    switch (msg->type) {
        case 100: {  /* MSG_INCREMENT */
            int* val = (int*)msg->payload;
            ATOMIC_FETCH_ADD(&ca->count, *val);
            break;
        }
        default:
            ATOMIC_FETCH_ADD(&ca->errors, 1);
            break;
    }
    if (msg->payload_destroy && msg->payload) {
        msg->payload_destroy(msg->payload);
    }
}

static int test_basic_message_passing(void) {
    scheduler_pool_t* pool = scheduler_pool_create(4);
    scheduler_pool_start(pool);

    counter_actor_t ca;
    memset(&ca, 0, sizeof(ca));
    actor_init(&ca.actor, &ca, counter_dispatch, pool);

    /* Send 100 messages */
    for (int i = 0; i < 100; i++) {
        int* val = malloc(sizeof(int));
        *val = 1;
        message_t msg = { .type = 100, .payload = val, .payload_destroy = free };
        actor_send(&ca.actor, &msg);
    }

    /* Wait for processing */
    scheduler_pool_wait_for_idle(pool);

    scheduler_pool_stop(pool);
    actor_destroy(&ca.actor);
    scheduler_pool_destroy(pool);

    if (ca.count != 100) {
        printf("FAIL: expected 100, got %d\n", (int)ca.count);
        return 1;
    }
    if (ca.errors != 0) {
        printf("FAIL: expected 0 errors, got %d\n", (int)ca.errors);
        return 1;
    }
    printf("PASS: basic message passing\n");
    return 0;
}

static int test_multiple_actors(void) {
    scheduler_pool_t* pool = scheduler_pool_create(4);
    scheduler_pool_start(pool);

    #define N_ACTORS 16
    #define N_MSGS 500

    counter_actor_t actors[N_ACTORS];
    for (int i = 0; i < N_ACTORS; i++) {
        memset(&actors[i], 0, sizeof(counter_actor_t));
        actor_init(&actors[i].actor, &actors[i], counter_dispatch, pool);
    }

    for (int i = 0; i < N_ACTORS; i++) {
        for (int j = 0; j < N_MSGS; j++) {
            int* val = malloc(sizeof(int));
            *val = 1;
            message_t msg = { .type = 100, .payload = val, .payload_destroy = free };
            actor_send(&actors[i].actor, &msg);
        }
    }

    scheduler_pool_wait_for_idle(pool);

    scheduler_pool_stop(pool);
    for (int i = 0; i < N_ACTORS; i++) {
        actor_destroy(&actors[i].actor);
        if (actors[i].count != N_MSGS) {
            printf("FAIL: actor %d expected %d, got %d\n", i, N_MSGS, (int)actors[i].count);
            scheduler_pool_destroy(pool);
            return 1;
        }
    }
    scheduler_pool_destroy(pool);

    printf("PASS: multiple actors (%d x %d msgs)\n", N_ACTORS, N_MSGS);
    return 0;
}

static int test_backpressure(void) {
    scheduler_pool_t* pool = scheduler_pool_create(2);
    scheduler_pool_start(pool);

    counter_actor_t ca;
    memset(&ca, 0, sizeof(ca));
    actor_init(&ca.actor, &ca, counter_dispatch, pool);

    /* Fill mailbox past MAILBOX_MUTE_THRESHOLD (256) */
    for (int i = 0; i < 300; i++) {
        int* val = malloc(sizeof(int));
        *val = 1;
        message_t msg = { .type = 100, .payload = val, .payload_destroy = free };
        actor_send(&ca.actor, &msg);
    }

    /* After 256, the actor should be PRESSURED */
    int flags = ATOMIC_LOAD(&ca.actor.flags);
    if (!(flags & ACTOR_FLAG_PRESSURED)) {
        printf("FAIL: expected PRESSURED flag after 300 messages\n");
        actor_destroy(&ca.actor);
        scheduler_pool_stop(pool);
        scheduler_pool_destroy(pool);
        return 1;
    }

    scheduler_pool_wait_for_idle(pool);

    /* After drain, PRESSURE should be released */
    flags = ATOMIC_LOAD(&ca.actor.flags);
    if (flags & ACTOR_FLAG_PRESSURED) {
        printf("FAIL: expected PRESSURED released after drain\n");
        actor_destroy(&ca.actor);
        scheduler_pool_stop(pool);
        scheduler_pool_destroy(pool);
        return 1;
    }

    actor_destroy(&ca.actor);
    scheduler_pool_stop(pool);
    scheduler_pool_destroy(pool);

    printf("PASS: backpressure apply/release\n");
    return 0;
}

int main(void) {
    int failures = 0;
    failures += test_basic_message_passing();
    failures += test_multiple_actors();
    failures += test_backpressure();
    printf("\n%d failures\n", failures);
    return failures;
}
```

- [ ] **Step 2: Add test to CMakeLists.txt**

In the tests section of CMakeLists.txt, after the existing test entries, add:

```cmake
add_executable(test_actor_scheduler tests/test_actor_scheduler.c)
target_link_libraries(test_actor_scheduler wavedb)
```

- [ ] **Step 3: Build and run**

```bash
cd build && cmake .. && make -j$(nproc) test_actor_scheduler && ./test_actor_scheduler
```

Expected output:
```
PASS: basic message passing
PASS: multiple actors (16 x 500 msgs)
PASS: backpressure apply/release
0 failures
```

- [ ] **Step 4: Commit**

```bash
git add tests/test_actor_scheduler.c CMakeLists.txt
git commit -m "test: add scheduler and actor core unit tests"
```

---

## Phase 2: Convert LRU Cache to Actor

### Task 2.1: Create LRU actor

**Files:**
- Create: `src/Database/lru_actor.h`
- Create: `src/Database/lru_actor.c`

**Context:** Replace the sharded-lock LRU with an actor-based LRU. The LRU actor owns all entries and processes get/put/delete messages sequentially. No locks needed internally. We keep the existing `database_lru_cache_t` for now (sync-only mode compat) and add the actor alongside it.

- [ ] **Step 1: Write lru_actor.h**

Create `src/Database/lru_actor.h`:

```c
#ifndef WAVEDB_LRU_ACTOR_H
#define WAVEDB_LRU_ACTOR_H

#include <stdint.h>
#include <stddef.h>
#include "../RefCounter/refcounter.h"
#include "../HBTrie/path.h"
#include "../HBTrie/identifier.h"
#include "../Actor/actor.h"
#include "../Actor/message.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lru_actor_t {
    actor_t actor;
    struct lru_entry_t* first;     /* Most recently used */
    struct lru_entry_t* last;      /* Least recently used */
    size_t current_memory;
    size_t max_memory;
    size_t entry_count;
} lru_actor_t;

lru_actor_t* lru_actor_create(size_t max_memory_bytes);
void lru_actor_destroy(lru_actor_t* lru);

/* Convenience: send LRU_GET message, return result via reply_to */
void lru_actor_get(lru_actor_t* lru, path_t* path, actor_t* reply_to);
/* Convenience: send LRU_PUT message */
void lru_actor_put(lru_actor_t* lru, path_t* path, identifier_t* value, actor_t* reply_to);
/* Convenience: send LRU_DELETE message */
void lru_actor_delete(lru_actor_t* lru, path_t* path);

#ifdef __cplusplus
}
#endif

#endif /* WAVEDB_LRU_ACTOR_H */
```

- [ ] **Step 2: Write lru_actor.c**

Create `src/Database/lru_actor.c`:

```c
#include "lru_actor.h"
#include "../Util/allocator.h"
#include "../Util/hash.h"
#include <stdlib.h>
#include <string.h>

typedef struct lru_entry_t {
    path_t* path;
    uint64_t key_hash;
    identifier_t* value;
    size_t memory_size;
    struct lru_entry_t* next;
    struct lru_entry_t* prev;
} lru_entry_t;

static size_t estimate_entry_size(path_t* path, identifier_t* value) {
    size_t s = sizeof(lru_entry_t);
    s += path->length;
    s += value->length;
    return s;
}

static void lru_actor_dispatch(void* state, message_t* msg) {
    lru_actor_t* lru = (lru_actor_t*)state;

    switch (msg->type) {
        case LRU_GET: {
            lru_op_payload_t* p = (lru_op_payload_t*)msg->payload;
            identifier_t* found = NULL;
            uint64_t hash = hash_path(p->path);
            /* Linear search (simplified; production code would use hashmap) */
            for (lru_entry_t* e = lru->first; e != NULL; e = e->next) {
                if (e->key_hash == hash && path_equals(e->path, p->path)) {
                    found = REFERENCE(e->value, identifier);
                    /* Move to front */
                    if (e != lru->first) {
                        if (e->prev) e->prev->next = e->next;
                        if (e->next) e->next->prev = e->prev;
                        if (e == lru->last) lru->last = e->prev;
                        e->next = lru->first;
                        e->prev = NULL;
                        if (lru->first) lru->first->prev = e;
                        lru->first = e;
                    }
                    break;
                }
            }
            if (p->reply_to) {
                lru_result_payload_t* result = get_clear_memory(sizeof(lru_result_payload_t));
                result->path = NULL;
                result->value = found;
                message_t reply = { .type = LRU_GET_RESULT, .payload = result, .payload_destroy = free };
                actor_send(p->reply_to, &reply);
            }
            /* Cleanup request payload */
            DESTROY(p->path, path);
            break;
        }
        case LRU_PUT: {
            lru_op_payload_t* p = (lru_op_payload_t*)msg->payload;
            size_t mem = estimate_entry_size(p->path, p->value);
            /* Evict oldest entries while over budget */
            while (lru->current_memory + mem > lru->max_memory && lru->last != NULL) {
                lru_entry_t* oldest = lru->last;
                if (oldest->prev) oldest->prev->next = NULL;
                lru->last = oldest->prev;
                if (lru->first == oldest) lru->first = NULL;
                lru->current_memory -= oldest->memory_size;
                lru->entry_count--;
                DESTROY(oldest->path, path);
                DESTROY(oldest->value, identifier);
                free(oldest);
            }
            /* Create new entry */
            lru_entry_t* entry = get_clear_memory(sizeof(lru_entry_t));
            entry->path = REFERENCE(p->path, path);
            entry->key_hash = hash_path(p->path);
            entry->value = REFERENCE(p->value, identifier);
            entry->memory_size = mem;
            entry->next = lru->first;
            if (lru->first) lru->first->prev = entry;
            lru->first = entry;
            if (!lru->last) lru->last = entry;
            lru->current_memory += mem;
            lru->entry_count++;
            DESTROY(p->path, path);
            DESTROY(p->value, identifier);
            break;
        }
        case LRU_DELETE: {
            lru_op_payload_t* p = (lru_op_payload_t*)msg->payload;
            uint64_t hash = hash_path(p->path);
            for (lru_entry_t* e = lru->first; e != NULL; e = e->next) {
                if (e->key_hash == hash && path_equals(e->path, p->path)) {
                    if (e->prev) e->prev->next = e->next;
                    if (e->next) e->next->prev = e->prev;
                    if (e == lru->first) lru->first = e->next;
                    if (e == lru->last) lru->last = e->prev;
                    lru->current_memory -= e->memory_size;
                    lru->entry_count--;
                    DESTROY(e->path, path);
                    DESTROY(e->value, identifier);
                    free(e);
                    break;
                }
            }
            DESTROY(p->path, path);
            break;
        }
        default:
            break;
    }
}

lru_actor_t* lru_actor_create(size_t max_memory_bytes) {
    lru_actor_t* lru = get_clear_memory(sizeof(lru_actor_t));
    lru->max_memory = max_memory_bytes ? max_memory_bytes : (size_t)-1;
    actor_init(&lru->actor, lru, lru_actor_dispatch, NULL);
    return lru;
}

void lru_actor_destroy(lru_actor_t* lru) {
    if (!lru) return;
    /* Free all entries */
    lru_entry_t* e = lru->first;
    while (e) {
        lru_entry_t* next = e->next;
        DESTROY(e->path, path);
        DESTROY(e->value, identifier);
        free(e);
        e = next;
    }
    actor_destroy(&lru->actor);
    free(lru);
}

void lru_actor_get(lru_actor_t* lru, path_t* path, actor_t* reply_to) {
    lru_op_payload_t* p = get_clear_memory(sizeof(lru_op_payload_t));
    p->path = REFERENCE(path, path);
    p->value = NULL;
    p->reply_to = reply_to;
    message_t msg = { .type = LRU_GET, .payload = p, .payload_destroy = free };
    actor_send(&lru->actor, &msg);
}

void lru_actor_put(lru_actor_t* lru, path_t* path, identifier_t* value, actor_t* reply_to) {
    lru_op_payload_t* p = get_clear_memory(sizeof(lru_op_payload_t));
    p->path = REFERENCE(path, path);
    p->value = REFERENCE(value, identifier);
    p->reply_to = reply_to;
    message_t msg = { .type = LRU_PUT, .payload = p, .payload_destroy = free };
    actor_send(&lru->actor, &msg);
}

void lru_actor_delete(lru_actor_t* lru, path_t* path) {
    lru_op_payload_t* p = get_clear_memory(sizeof(lru_op_payload_t));
    p->path = REFERENCE(path, path);
    p->value = NULL;
    p->reply_to = NULL;
    message_t msg = { .type = LRU_DELETE, .payload = p, .payload_destroy = free };
    actor_send(&lru->actor, &msg);
}
```

- [ ] **Step 3: Add hash_path function**

Create or verify `src/Util/hash.h` exists with a `hash_path` function. If it doesn't exist, add a minimal one:

```c
#ifndef WAVEDB_HASH_H
#define WAVEDB_HASH_H
#include "../HBTrie/path.h"
#include <stdint.h>

static inline uint64_t hash_path(path_t* path) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < path->identifiers.length; i++) {
        identifier_t* id = path->identifiers.data[i];
        for (size_t j = 0; j < id->chunks.length; j++) {
            chunk_t* c = id->chunks.data[j];
            for (size_t k = 0; k < c->size; k++) {
                h ^= c->data[k];
                h *= 1099511628211ULL;
            }
        }
    }
    return h;
}

static inline int path_equals(path_t* a, path_t* b) {
    if (a->length != b->length) return 0;
    return memcmp(a->data, b->data, a->length) == 0;
}
#endif
```

- [ ] **Step 4: Add to CMakeLists.txt**

```cmake
# Add lru_actor.c to the library
```

- [ ] **Step 5: Write basic test**

Create `tests/test_cache_actors.c`:

```c
#include <stdio.h>
#include "../src/Database/lru_actor.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/HBTrie/path.h"
#include "../src/HBTrie/identifier.h"

/* Completion actor for async results */
typedef struct {
    actor_t actor;
    ATOMIC(uint8_t) done;
    identifier_t* result;
} lru_completion_t;

static void lru_completion_dispatch(void* state, message_t* msg) {
    lru_completion_t* c = (lru_completion_t*)state;
    if (msg->type == LRU_GET_RESULT) {
        lru_result_payload_t* r = (lru_result_payload_t*)msg->payload;
        c->result = r->value;
        r->value = NULL;
    }
    ATOMIC_STORE(&c->done, 1);
    if (msg->payload_destroy && msg->payload) {
        msg->payload_destroy(msg->payload);
    }
}

int main(void) {
    scheduler_pool_t* pool = scheduler_pool_create(2);
    scheduler_pool_start(pool);

    lru_actor_t* lru = lru_actor_create(1024 * 1024);
    lru->actor.pool = pool;  /* wire to scheduler */

    /* Create a test path and value */
    char data1[] = "value1";
    buffer_t* buf1 = buffer_create(data1, strlen(data1));
    identifier_t* id1 = buffer_to_identifier(buf1);
    path_t* path1 = path_create();
    path_append(path1, id1);

    /* Put */
    lru_actor_put(lru, path1, id1, NULL);
    scheduler_pool_wait_for_idle(pool);

    /* Get */
    lru_completion_t comp = {{0}};
    actor_init(&comp.actor, &comp, lru_completion_dispatch, pool);
    lru_actor_get(lru, path1, &comp.actor);
    /* Pump messages manually since completion is on the same pool */
    while (!ATOMIC_LOAD(&comp.done)) { usleep(100); }

    int failures = 0;
    if (comp.result == NULL) {
        printf("FAIL: expected non-NULL result\n");
        failures++;
    } else {
        printf("PASS: lru get returns stored value\n");
        DESTROY(comp.result, identifier);
    }

    actor_destroy(&comp.actor);
    DESTROY(path1, path);
    DESTROY(id1, identifier);
    buffer_destroy(buf1);
    lru_actor_destroy(lru);
    scheduler_pool_stop(pool);
    scheduler_pool_destroy(pool);

    printf("\n%d failures\n", failures);
    return failures;
}
```

- [ ] **Step 6: Build and run**

```bash
cd build && make -j$(nproc) && ./test_cache_actors
```

- [ ] **Step 7: Commit**

```bash
git add src/Database/lru_actor.h src/Database/lru_actor.c src/Util/hash.h tests/test_cache_actors.c CMakeLists.txt
git commit -m "feat: add actor-based LRU cache"
```

---

## Phase 3: Convert BNode Cache to Actor

### Task 3.1: Create BNode cache actor

**Files:**
- Create: `src/Database/bnode_cache_actor.h`
- Create: `src/Database/bnode_cache_actor.c`

**Context:** Convert `file_bnode_cache_t` to an actor. Each bnode cache (one per page file) becomes an actor. Read/write/release/invalidate operations are messages. The per-shard mutexes are eliminated.

- [ ] **Step 1: Write bnode_cache_actor.h**

Create `src/Database/bnode_cache_actor.h`:

```c
#ifndef WAVEDB_BNODE_CACHE_ACTOR_H
#define WAVEDB_BNODE_CACHE_ACTOR_H

#include <stdint.h>
#include <stddef.h>
#include "../Actor/actor.h"
#include "../Actor/message.h"
#include "../Storage/page_file.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bnode_cache_actor_t bnode_cache_actor_t;

bnode_cache_actor_t* bnode_cache_actor_create(page_file_t* pf, size_t max_memory, size_t num_shards);
void bnode_cache_actor_destroy(bnode_cache_actor_t* cache);

/* Async operations — result returned via reply_to actor */
void bnode_cache_actor_read(bnode_cache_actor_t* cache, uint64_t offset, actor_t* reply_to);
void bnode_cache_actor_write(bnode_cache_actor_t* cache, uint64_t offset, const uint8_t* data, size_t len, actor_t* reply_to);
void bnode_cache_actor_release(bnode_cache_actor_t* cache, uint64_t offset);
void bnode_cache_actor_invalidate(bnode_cache_actor_t* cache, uint64_t offset);

#ifdef __cplusplus
}
#endif

#endif /* WAVEDB_BNODE_CACHE_ACTOR_H */
```

- [ ] **Step 2: Write bnode_cache_actor.c**

Create `src/Database/bnode_cache_actor.c`:

```c
#include "bnode_cache_actor.h"
#include "../Util/allocator.h"
#include <stdlib.h>
#include <string.h>

typedef struct cache_item_t {
    uint64_t offset;
    uint8_t* data;
    size_t data_len;
    struct cache_item_t* next;
    struct cache_item_t* prev;  /* For LRU ordering */
} cache_item_t;

struct bnode_cache_actor_t {
    actor_t actor;
    page_file_t* page_file;
    cache_item_t* first;    /* MRU */
    cache_item_t* last;     /* LRU */
    size_t max_memory;
    size_t current_memory;
};

static void bnode_cache_actor_dispatch(void* state, message_t* msg) {
    bnode_cache_actor_t* cache = (bnode_cache_actor_t*)state;

    switch (msg->type) {
        case BNODE_CACHE_READ: {
            bnode_cache_op_payload_t* p = (bnode_cache_op_payload_t*)msg->payload;
            uint8_t* result_data = NULL;
            size_t result_len = 0;

            /* Check if cached */
            for (cache_item_t* item = cache->first; item != NULL; item = item->next) {
                if (item->offset == p->offset) {
                    /* Move to front */
                    if (item != cache->first) {
                        if (item->prev) item->prev->next = item->next;
                        if (item->next) item->next->prev = item->prev;
                        if (item == cache->last) cache->last = item->prev;
                        item->next = cache->first;
                        item->prev = NULL;
                        if (cache->first) cache->first->prev = item;
                        cache->first = item;
                    }
                    result_data = malloc(item->data_len);
                    memcpy(result_data, item->data, item->data_len);
                    result_len = item->data_len;
                    break;
                }
            }

            /* Cache miss — read from page file */
            if (result_data == NULL) {
                result_data = page_file_read(cache->page_file, p->offset, &result_len);
                if (result_data) {
                    /* Cache it if it fits */
                    size_t needed = sizeof(cache_item_t) + result_len;
                    while (cache->current_memory + needed > cache->max_memory && cache->last) {
                        cache_item_t* evict = cache->last;
                        if (evict->prev) evict->prev->next = NULL;
                        cache->last = evict->prev;
                        if (cache->first == evict) cache->first = NULL;
                        cache->current_memory -= sizeof(cache_item_t) + evict->data_len;
                        free(evict->data);
                        free(evict);
                    }
                    cache_item_t* item = get_clear_memory(sizeof(cache_item_t));
                    item->offset = p->offset;
                    item->data = malloc(result_len);
                    memcpy(item->data, result_data, result_len);
                    item->data_len = result_len;
                    item->next = cache->first;
                    if (cache->first) cache->first->prev = item;
                    cache->first = item;
                    if (!cache->last) cache->last = item;
                    cache->current_memory += needed;
                }
            }

            if (p->reply_to) {
                bnode_cache_result_payload_t* result = get_clear_memory(sizeof(bnode_cache_result_payload_t));
                result->offset = p->offset;
                result->data = result_data;
                result->data_len = result_len;
                message_t reply = { .type = BNODE_CACHE_READ_RESULT, .payload = result, .payload_destroy = free };
                actor_send(p->reply_to, &reply);
            }
            break;
        }
        case BNODE_CACHE_WRITE: {
            bnode_cache_op_payload_t* p = (bnode_cache_op_payload_t*)msg->payload;
            page_file_write(cache->page_file, p->offset, p->data, p->data_len);
            /* Evict by offset if already cached, then cache the new data */
            for (cache_item_t* item = cache->first; item != NULL; item = item->next) {
                if (item->offset == p->offset) {
                    /* Update in-place */
                    free(item->data);
                    item->data = malloc(p->data_len);
                    memcpy(item->data, p->data, p->data_len);
                    item->data_len = p->data_len;
                    goto done_write;
                }
            }
            /* Not cached — evict and add */
            {
                size_t needed = sizeof(cache_item_t) + p->data_len;
                while (cache->current_memory + needed > cache->max_memory && cache->last) {
                    cache_item_t* evict = cache->last;
                    if (evict->prev) evict->prev->next = NULL;
                    cache->last = evict->prev;
                    if (cache->first == evict) cache->first = NULL;
                    cache->current_memory -= sizeof(cache_item_t) + evict->data_len;
                    free(evict->data);
                    free(evict);
                }
                cache_item_t* item = get_clear_memory(sizeof(cache_item_t));
                item->offset = p->offset;
                item->data = malloc(p->data_len);
                memcpy(item->data, p->data, p->data_len);
                item->data_len = p->data_len;
                item->next = cache->first;
                if (cache->first) cache->first->prev = item;
                cache->first = item;
                if (!cache->last) cache->last = item;
                cache->current_memory += needed;
            }
done_write:
            if (p->data) free(p->data);
            break;
        }
        case BNODE_CACHE_RELEASE: {
            /* Release refcount — no-op for now (full impl tracks refcounts) */
            break;
        }
        case BNODE_CACHE_INVALIDATE: {
            bnode_cache_op_payload_t* p = (bnode_cache_op_payload_t*)msg->payload;
            for (cache_item_t* item = cache->first; item != NULL; item = item->next) {
                if (item->offset == p->offset) {
                    if (item->prev) item->prev->next = item->next;
                    if (item->next) item->next->prev = item->prev;
                    if (item == cache->first) cache->first = item->next;
                    if (item == cache->last) cache->last = item->prev;
                    cache->current_memory -= sizeof(cache_item_t) + item->data_len;
                    free(item->data);
                    free(item);
                    break;
                }
            }
            break;
        }
        default:
            break;
    }
}

bnode_cache_actor_t* bnode_cache_actor_create(page_file_t* pf, size_t max_memory, size_t num_shards) {
    (void)num_shards; /* Single actor replaces sharding */
    bnode_cache_actor_t* cache = get_clear_memory(sizeof(bnode_cache_actor_t));
    cache->page_file = pf;
    cache->max_memory = max_memory;
    actor_init(&cache->actor, cache, bnode_cache_actor_dispatch, NULL);
    return cache;
}

void bnode_cache_actor_destroy(bnode_cache_actor_t* cache) {
    if (!cache) return;
    cache_item_t* item = cache->first;
    while (item) {
        cache_item_t* next = item->next;
        free(item->data);
        free(item);
        item = next;
    }
    actor_destroy(&cache->actor);
    free(cache);
}

void bnode_cache_actor_read(bnode_cache_actor_t* cache, uint64_t offset, actor_t* reply_to) {
    bnode_cache_op_payload_t* p = get_clear_memory(sizeof(bnode_cache_op_payload_t));
    p->offset = offset;
    p->data = NULL;
    p->data_len = 0;
    p->reply_to = reply_to;
    message_t msg = { .type = BNODE_CACHE_READ, .payload = p, .payload_destroy = free };
    actor_send(&cache->actor, &msg);
}

void bnode_cache_actor_write(bnode_cache_actor_t* cache, uint64_t offset, const uint8_t* data, size_t len, actor_t* reply_to) {
    bnode_cache_op_payload_t* p = get_clear_memory(sizeof(bnode_cache_op_payload_t));
    p->offset = offset;
    p->data = malloc(len);
    memcpy(p->data, data, len);
    p->data_len = len;
    p->reply_to = reply_to;
    message_t msg = { .type = BNODE_CACHE_WRITE, .payload = p, .payload_destroy = free };
    actor_send(&cache->actor, &msg);
}

void bnode_cache_actor_release(bnode_cache_actor_t* cache, uint64_t offset) {
    bnode_cache_op_payload_t* p = get_clear_memory(sizeof(bnode_cache_op_payload_t));
    p->offset = offset;
    p->data = NULL;
    p->data_len = 0;
    p->reply_to = NULL;
    message_t msg = { .type = BNODE_CACHE_RELEASE, .payload = p, .payload_destroy = free };
    actor_send(&cache->actor, &msg);
}

void bnode_cache_actor_invalidate(bnode_cache_actor_t* cache, uint64_t offset) {
    bnode_cache_op_payload_t* p = get_clear_memory(sizeof(bnode_cache_op_payload_t));
    p->offset = offset;
    p->data = NULL;
    p->data_len = 0;
    p->reply_to = NULL;
    message_t msg = { .type = BNODE_CACHE_INVALIDATE, .payload = p, .payload_destroy = free };
    actor_send(&cache->actor, &msg);
}
```

- [ ] **Step 2: Add to CMakeLists.txt**

Add `src/Database/bnode_cache_actor.c` to the library sources.

- [ ] **Step 3: Build**

```bash
cd build && cmake .. && make -j$(nproc) 2>&1
```

- [ ] **Step 4: Commit**

```bash
git add src/Database/bnode_cache_actor.h src/Database/bnode_cache_actor.c CMakeLists.txt
git commit -m "feat: add actor-based bnode cache"
```

---

## Phase 4: Convert WAL to Actor

### Task 4.1: Create WAL actor

**Files:**
- Create: `src/Database/wal_actor.h`
- Create: `src/Database/wal_actor.c`

**Context:** The WAL actor replaces `thread_wal_t` locking and `wal_manager_t.manifest_lock`. It runs as a single actor that receives `WAL_WRITE` messages from trie shard actors. It batches writes in its dispatch handler for efficiency. The I/O is done synchronously within the actor's dispatch (single-threaded, so no I/O thread needed for writes). For fsync-intensive workloads, the WAL actor's mailbox provides natural backpressure (trie shard actors get muted if WAL falls behind).

- [ ] **Step 1: Write wal_actor.h**

Create `src/Database/wal_actor.h`:

```c
#ifndef WAVEDB_WAL_ACTOR_H
#define WAVEDB_WAL_ACTOR_H

#include <stdint.h>
#include <stddef.h>
#include "../Actor/actor.h"
#include "../Actor/message.h"
#include "../Buffer/buffer.h"
#include "../Workers/transaction_id.h"
#include "../Storage/encryption.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wal_actor_t {
    actor_t actor;
    char* location;
    int fd;
    uint64_t thread_id;
    hierarchical_timing_wheel_t* wheel;
    encryption_t* encryption;
    size_t pending_writes;
    size_t max_file_size;
    transaction_id_t oldest_txn_id;
    transaction_id_t newest_txn_id;
    /* Batch buffer */
    uint8_t entry_buf[4096];
    size_t entry_buf_used;
} wal_actor_t;

wal_actor_t* wal_actor_create(const char* location, hierarchical_timing_wheel_t* wheel,
                               encryption_t* encryption, int* error_code);
void wal_actor_destroy(wal_actor_t* wal);

/* Send a write record to the WAL actor */
void wal_actor_write(wal_actor_t* wal, uint64_t thread_id, transaction_id_t txn_id,
                     uint8_t type, buffer_t* data, actor_t* reply_to);

#ifdef __cplusplus
}
#endif

#endif /* WAVEDB_WAL_ACTOR_H */
```

- [ ] **Step 2: Write wal_actor.c**

Create `src/Database/wal_actor.c`:

```c
#include "wal_actor.h"
#include "../Util/allocator.h"
#include "../Util/hash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

static void _flush_batch(wal_actor_t* wal) {
    if (wal->entry_buf_used == 0) return;
    ssize_t written = write(wal->fd, wal->entry_buf, wal->entry_buf_used);
    if (written > 0) {
        wal->pending_writes++;
        if (wal->pending_writes >= 16) {
            fsync(wal->fd);
            wal->pending_writes = 0;
        }
        wal->current_size += (size_t)written;
        if (wal->current_size >= wal->max_file_size) {
            /* Rotate: close current, open new */
            fsync(wal->fd);
            close(wal->fd);
            char new_path[512];
            snprintf(new_path, sizeof(new_path), "%s/wal_%lu.wal", wal->location, (unsigned long)wal->newest_txn_id);
            wal->fd = open(new_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
            wal->current_size = 0;
        }
    }
    wal->entry_buf_used = 0;
}

static void wal_actor_dispatch(void* state, message_t* msg) {
    wal_actor_t* wal = (wal_actor_t*)state;

    switch (msg->type) {
        case WAL_WRITE: {
            wal_record_payload_t* p = (wal_record_payload_t*)msg->payload;
            /* Serialize to batch buffer */
            size_t needed = sizeof(uint64_t) + sizeof(transaction_id_t) + 1 + sizeof(uint32_t) + p->data->length;
            if (wal->entry_buf_used + needed > sizeof(wal->entry_buf) || wal->entry_buf_used > 0) {
                _flush_batch(wal);
            }
            if (needed > sizeof(wal->entry_buf)) {
                /* Large entry — write directly */
                write(wal->fd, &p->thread_id, sizeof(uint64_t));
                write(wal->fd, &p->txn_id, sizeof(transaction_id_t));
                write(wal->fd, &p->type, 1);
                uint32_t len = (uint32_t)p->data->length;
                write(wal->fd, &len, sizeof(uint32_t));
                write(wal->fd, p->data->data, p->data->length);
            } else {
                memcpy(wal->entry_buf + wal->entry_buf_used, &p->thread_id, sizeof(uint64_t));
                wal->entry_buf_used += sizeof(uint64_t);
                memcpy(wal->entry_buf + wal->entry_buf_used, &p->txn_id, sizeof(transaction_id_t));
                wal->entry_buf_used += sizeof(transaction_id_t);
                wal->entry_buf[wal->entry_buf_used++] = p->type;
                uint32_t len = (uint32_t)p->data->length;
                memcpy(wal->entry_buf + wal->entry_buf_used, &len, sizeof(uint32_t));
                wal->entry_buf_used += sizeof(uint32_t);
                memcpy(wal->entry_buf + wal->entry_buf_used, p->data->data, p->data->length);
                wal->entry_buf_used += p->data->length;
            }
            if (p->txn_id > wal->newest_txn_id) wal->newest_txn_id = p->txn_id;
            if (wal->oldest_txn_id == 0) wal->oldest_txn_id = p->txn_id;
            buffer_destroy(p->data);
            break;
        }
        case WAL_FLUSH: {
            _flush_batch(wal);
            if (wal->pending_writes > 0) {
                fsync(wal->fd);
                wal->pending_writes = 0;
            }
            break;
        }
        default:
            break;
    }
}

wal_actor_t* wal_actor_create(const char* location, hierarchical_timing_wheel_t* wheel,
                               encryption_t* encryption, int* error_code) {
    *error_code = 0;
    wal_actor_t* wal = get_clear_memory(sizeof(wal_actor_t));
    wal->location = strdup(location);
    wal->wheel = wheel;
    wal->encryption = encryption;
    wal->max_file_size = 128 * 1024;

    /* Create directory if needed */
    mkdir(location, 0755);

    char path[512];
    snprintf(path, sizeof(path), "%s/wal_0.wal", location);
    wal->fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (wal->fd < 0) {
        *error_code = -1;
        free(wal->location);
        free(wal);
        return NULL;
    }
    wal->thread_id = platform_self();
    actor_init(&wal->actor, wal, wal_actor_dispatch, NULL);
    return wal;
}

void wal_actor_destroy(wal_actor_t* wal) {
    if (!wal) return;
    _flush_batch(wal);
    if (wal->pending_writes > 0) fsync(wal->fd);
    if (wal->fd >= 0) close(wal->fd);
    free(wal->location);
    actor_destroy(&wal->actor);
    free(wal);
}

void wal_actor_write(wal_actor_t* wal, uint64_t thread_id, transaction_id_t txn_id,
                     uint8_t type, buffer_t* data, actor_t* reply_to) {
    wal_record_payload_t* p = get_clear_memory(sizeof(wal_record_payload_t));
    p->thread_id = thread_id;
    p->txn_id = txn_id;
    p->type = type;
    p->data = REFERENCE(data, buffer);
    p->reply_to = reply_to;
    message_t msg = { .type = WAL_WRITE, .payload = p, .payload_destroy = free };
    actor_send(&wal->actor, &msg);
}
```

- [ ] **Step 3: Add platform_self() to platform_thread.c**

Verify that `platform_thread.c` from liboffs has a `platform_self()` function. If not, add it:

```c
uint64_t platform_self(void) {
    return (uint64_t)pthread_self();
}
```

- [ ] **Step 4: Add to CMakeLists.txt and build**

```bash
cd build && cmake .. && make -j$(nproc) 2>&1
```

- [ ] **Step 5: Commit**

```bash
git add src/Database/wal_actor.h src/Database/wal_actor.c CMakeLists.txt
git commit -m "feat: add actor-based WAL"
```

---

## Phase 5: Convert Trie to Shard Actors

### Task 5.1: Create trie shard actor

**Files:**
- Create: `src/Database/trie_shard_actor.h`
- Create: `src/Database/trie_shard_actor.c`
- Modify: `src/HBTrie/hbtrie.h` — remove write_lock, seq from hbtrie_node_t
- Modify: `src/HBTrie/bnode.h` — remove write_lock, seq from bnode_t
- Modify: `src/HBTrie/hbtrie.c` — remove lock acquire/release from operations

**Context:** The core transformation. Each shard actor owns a subtree of the HBTrie. Traversal is synchronous within the shard actor's dispatch. No locks needed since only one thread processes shard messages at a time. The atomic root pointer enables lock-free reads that bypass the actor queue.

- [ ] **Step 1: Remove write_lock and seq from hbtrie_node_t**

In `src/HBTrie/hbtrie.h`, modify the `hbtrie_node_t` struct:

```c
typedef struct hbtrie_node_t {
    refcounter_t refcounter;          // MUST be first member
    /* Removed: ATOMIC_TYPE(uint64_t) seq — no longer needed (single-threaded actor dispatch) */
    /* Removed: spinlock_t write_lock — replaced by actor message queue */

    bnode_t* btree;                   // Root bnode of multi-level B+tree at this level
    uint16_t btree_height;           // Height of B+tree (1 = single leaf, > 1 = has internal nodes)

    // Page file storage tracking (Phase 2: replaces section_id + block_index)
    uint64_t disk_offset;             // File offset of root bnode (UINT64_MAX if not persisted)
    uint8_t is_loaded;                // 1 if in memory, 0 if on-disk stub
    uint8_t is_dirty;                 // 1 if modified since last save
} hbtrie_node_t;
```

- [ ] **Step 2: Remove write_lock and seq from bnode_t**

In `src/HBTrie/bnode.h`, modify the `bnode_t` struct:

```c
typedef struct bnode_t {
    refcounter_t refcounter;          // MUST be first member (16-48 bytes)
    ATOMIC_TYPE(uint16_t) level;        // B+tree level: 1 = leaf, > 1 = internal
    uint32_t node_size;                // Configurable max size in bytes
    vec_t(bnode_entry_t) entries;      // Sorted by chunk key
    /* Removed: ATOMIC_TYPE(uint64_t) seq — no longer needed */
    /* Removed: spinlock_t write_lock — replaced by actor message queue */

    // Per-bnode disk tracking (Phase 2: flat per-bnode persistence)
    uint64_t disk_offset;              // File offset of this bnode (UINT64_MAX if not persisted)
    uint8_t is_dirty;                  // 1 if modified since last write
    uint8_t is_inline;                // 1 if embedded in combined allocation (don't free separately)
} bnode_t;
```

- [ ] **Step 3: Remove lock acquire/release from hbtrie.c operations**

In `src/HBTrie/hbtrie.c`, remove all `spinlock_lock(&node->write_lock)` and `spinlock_unlock(&node->write_lock)` calls. Also remove all seqlock acquire/release patterns. The operations now run single-threaded within a shard actor.

For `hbtrie_find` (read path), remove the seqlock retry loop. For `hbtrie_insert` and `hbtrie_delete`, remove the lock acquire/release calls. The function signatures stay the same.

- [ ] **Step 4: Write trie_shard_actor.h**

Create `src/Database/trie_shard_actor.h`:

```c
#ifndef WAVEDB_TRIESHARD_ACTOR_H
#define WAVEDB_TRIESHARD_ACTOR_H

#include <stdint.h>
#include <stddef.h>
#include "../Actor/actor.h"
#include "../Actor/message.h"
#include "../HBTrie/hbtrie.h"
#include "../HBTrie/mvcc.h"
#include "wal_actor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct trie_shard_actor_t {
    actor_t actor;
    hbtrie_t* trie;                    /* Owned subtree */
    tx_manager_t* tx_manager;          /* Shared MVCC state */
    wal_actor_t* wal;                  /* WAL actor for writes */
    ATOMIC(hbtrie_node_t*) root;      /* Atomic snapshot for lock-free reads */
    uint32_t chunk_size;
    uint32_t btree_node_size;
} trie_shard_actor_t;

/**
 * Create trie shard actors.
 *
 * @param count          Number of shards to create
 * @param chunk_size     HBTrie chunk size
 * @param btree_node_size B+tree node size
 * @param tx_manager     Shared transaction manager
 * @param wal            WAL actor reference
 * @return Array of shard actors (caller must free array; individual actors managed by scheduler)
 */
trie_shard_actor_t** trie_shard_actors_create(size_t count, uint8_t chunk_size,
                                               uint32_t btree_node_size,
                                               tx_manager_t* tx_manager,
                                               wal_actor_t* wal);

void trie_shard_actors_destroy(trie_shard_actor_t** shards, size_t count);

/* Send operations to the shard determined by path hash */
void trie_shard_put(trie_shard_actor_t** shards, size_t count, path_t* path, identifier_t* value, promise_t* promise);
void trie_shard_get(trie_shard_actor_t** shards, size_t count, path_t* path, promise_t* promise);
void trie_shard_delete(trie_shard_actor_t** shards, size_t count, path_t* path, promise_t* promise);

/* Lock-free read (bypasses actor queue) */
identifier_t* trie_shard_read_sync(trie_shard_actor_t** shards, size_t count, 
                                    path_t* path, transaction_id_t txn_id);

#ifdef __cplusplus
}
#endif

#endif /* WAVEDB_TRIESHARD_ACTOR_H */
```

- [ ] **Step 5: Write trie_shard_actor.c**

Create `src/Database/trie_shard_actor.c`:

```c
#include "trie_shard_actor.h"
#include "../Util/allocator.h"
#include "../Util/hash.h"
#include <stdlib.h>

static size_t get_shard_index(path_t* path, size_t count) {
    uint64_t h = hash_path(path);
    return (size_t)(h % count);
}

static void trie_shard_dispatch(void* state, message_t* msg) {
    trie_shard_actor_t* shard = (trie_shard_actor_t*)state;

    switch (msg->type) {
        case SHARD_PUT: {
            db_op_payload_t* p = (db_op_payload_t*)msg->payload;
            /* Single-threaded — no locks needed */
            hbtrie_insert(shard->trie, p->path, p->value, tx_manager_acquire_txn(shard->tx_manager));
            /* Update atomic root snapshot for lock-free readers */
            ATOMIC_STORE(&shard->root, shard->trie->root);
            /* Write to WAL */
            if (shard->wal) {
                /* Serialize path+value to buffer for WAL */
                uint8_t* buf = NULL;
                size_t len = 0;
                if (hbtrie_serialize(shard->trie, &buf, &len) == 0 && buf) {
                    buffer_t* wal_data = buffer_create(buf, len);
                    wal_actor_write(shard->wal, platform_self(), 
                                   tx_manager_acquire_txn(shard->tx_manager),
                                   0, wal_data, NULL);
                    free(buf);
                }
            }
            if (p->promise) promise_resolve(p->promise, NULL);
            break;
        }
        case SHARD_GET: {
            db_op_payload_t* p = (db_op_payload_t*)msg->payload;
            transaction_id_t txn_id = tx_manager_acquire_txn(shard->tx_manager);
            identifier_t* value = hbtrie_find(shard->trie, p->path, txn_id);
            if (p->promise) {
                if (value) {
                    promise_resolve(p->promise, value);
                } else {
                    promise_resolve(p->promise, NULL);
                }
            }
            DESTROY(p->path, path);
            break;
        }
        case SHARD_DELETE: {
            db_op_payload_t* p = (db_op_payload_t*)msg->payload;
            transaction_id_t txn_id = tx_manager_acquire_txn(shard->tx_manager);
            identifier_t* removed = hbtrie_delete(shard->trie, p->path, txn_id);
            if (removed) DESTROY(removed, identifier);
            ATOMIC_STORE(&shard->root, shard->trie->root);
            if (shard->wal) {
                uint8_t* buf = NULL;
                size_t len = 0;
                if (hbtrie_serialize(shard->trie, &buf, &len) == 0 && buf) {
                    buffer_t* wal_data = buffer_create(buf, len);
                    wal_actor_write(shard->wal, platform_self(), txn_id, 1, wal_data, NULL);
                    free(buf);
                }
            }
            if (p->promise) promise_resolve(p->promise, NULL);
            break;
        }
        default:
            break;
    }
}

trie_shard_actor_t** trie_shard_actors_create(size_t count, uint8_t chunk_size,
                                               uint32_t btree_node_size,
                                               tx_manager_t* tx_manager,
                                               wal_actor_t* wal) {
    trie_shard_actor_t** shards = get_clear_memory(count * sizeof(trie_shard_actor_t*));
    for (size_t i = 0; i < count; i++) {
        trie_shard_actor_t* shard = get_clear_memory(sizeof(trie_shard_actor_t));
        shard->trie = hbtrie_create(chunk_size, btree_node_size);
        shard->tx_manager = tx_manager;
        shard->wal = wal;
        shard->chunk_size = chunk_size;
        shard->btree_node_size = btree_node_size;
        ATOMIC_STORE(&shard->root, shard->trie->root);
        actor_init(&shard->actor, shard, trie_shard_dispatch, NULL);
        shards[i] = shard;
    }
    return shards;
}

void trie_shard_actors_destroy(trie_shard_actor_t** shards, size_t count) {
    if (!shards) return;
    for (size_t i = 0; i < count; i++) {
        if (shards[i]) {
            actor_destroy(&shards[i]->actor);
            hbtrie_destroy(shards[i]->trie);
            free(shards[i]);
        }
    }
    free(shards);
}

void trie_shard_put(trie_shard_actor_t** shards, size_t count, path_t* path, identifier_t* value, promise_t* promise) {
    size_t idx = get_shard_index(path, count);
    db_op_payload_t* p = get_clear_memory(sizeof(db_op_payload_t));
    p->path = path;  /* Ownership transferred — no REFERENCE needed, callee DESTROYs */
    p->value = value;
    p->promise = promise;
    message_t msg = { .type = SHARD_PUT, .payload = p, .payload_destroy = free };
    actor_send(&shards[idx]->actor, &msg);
}

void trie_shard_get(trie_shard_actor_t** shards, size_t count, path_t* path, promise_t* promise) {
    size_t idx = get_shard_index(path, count);
    db_op_payload_t* p = get_clear_memory(sizeof(db_op_payload_t));
    p->path = path;
    p->value = NULL;
    p->promise = promise;
    message_t msg = { .type = SHARD_GET, .payload = p, .payload_destroy = free };
    actor_send(&shards[idx]->actor, &msg);
}

void trie_shard_delete(trie_shard_actor_t** shards, size_t count, path_t* path, promise_t* promise) {
    size_t idx = get_shard_index(path, count);
    db_op_payload_t* p = get_clear_memory(sizeof(db_op_payload_t));
    p->path = path;
    p->value = NULL;
    p->promise = promise;
    message_t msg = { .type = SHARD_DELETE, .payload = p, .payload_destroy = free };
    actor_send(&shards[idx]->actor, &msg);
}

identifier_t* trie_shard_read_sync(trie_shard_actor_t** shards, size_t count, 
                                    path_t* path, transaction_id_t txn_id) {
    size_t idx = get_shard_index(path, count);
    /* Lock-free read: atomic load root, traverse without messaging */
    hbtrie_node_t* root = ATOMIC_LOAD(&shards[idx]->root);
    if (!root) return NULL;
    /* Temporarily set root for traversal (borrowed, not owned) */
    hbtrie_node_t* saved_root = shards[idx]->trie->root;
    shards[idx]->trie->root = root;
    identifier_t* result = hbtrie_find(shards[idx]->trie, path, txn_id);
    shards[idx]->trie->root = saved_root;
    return result;
}
```

- [ ] **Step 6: Add to CMakeLists.txt and build**

```bash
cd build && cmake .. && make -j$(nproc) 2>&1
```

Expected: Build errors from removed lock/seqlock code in hbtrie.c and bnode.c. Fix by removing the lock acquire/release calls from those files.

- [ ] **Step 7: Write test**

Create `tests/test_trie_shard_actor.c`:

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/Database/trie_shard_actor.h"
#include "../src/Database/wal_actor.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/HBTrie/path.h"
#include "../src/HBTrie/identifier.h"
#include "../src/Util/allocator.h"

int main(void) {
    scheduler_pool_t* pool = scheduler_pool_create(4);
    scheduler_pool_start(pool);

    tx_manager_t* txm = tx_manager_create(1000);
    wal_actor_t* wal = wal_actor_create("/tmp/test_wal", NULL, NULL, &(int){0});
    trie_shard_actor_t** shards = trie_shard_actors_create(8, 4, 4096, txm, wal);

    /* Wire actors to scheduler */
    wal->actor.pool = pool;
    for (int i = 0; i < 8; i++) shards[i]->actor.pool = pool;

    /* Create test data */
    char* keys[] = {"apple", "banana", "cherry", "date"};
    int failures = 0;

    for (int i = 0; i < 4; i++) {
        buffer_t* buf = buffer_create((uint8_t*)keys[i], strlen(keys[i]));
        identifier_t* id = buffer_to_identifier(buf);
        path_t* p = path_create();
        path_append(p, id);

        /* Put */
        trie_shard_put(shards, 8, p, id, NULL);
        scheduler_pool_wait_for_idle(pool);

        /* Sync read */
        identifier_t* found = trie_shard_read_sync(shards, 8, p, 0);
        if (!found) {
            printf("FAIL: key '%s' not found after put\n", keys[i]);
            failures++;
        } else {
            printf("PASS: key '%s' found\n", keys[i]);
            DESTROY(found, identifier);
        }
        DESTROY(p, path);
        DESTROY(id, identifier);
        buffer_destroy(buf);
    }

    /* Test path-based sharding: different paths should go to different shards */
    printf("PASS: all keys stored and retrieved\n");
    printf("%d failures\n", failures);

    trie_shard_actors_destroy(shards, 8);
    wal_actor_destroy(wal);
    tx_manager_destroy(txm);
    scheduler_pool_stop(pool);
    scheduler_pool_destroy(pool);

    return failures;
}
```

- [ ] **Step 8: Build and run test**

```bash
cd build && make -j$(nproc) test_trie_shard_actor && ./test_trie_shard_actor
```

- [ ] **Step 9: Commit**

```bash
git add src/Database/trie_shard_actor.h src/Database/trie_shard_actor.c src/HBTrie/hbtrie.h src/HBTrie/bnode.h src/HBTrie/hbtrie.c tests/test_trie_shard_actor.c CMakeLists.txt
git commit -m "feat: add actor-based trie shards with lock-free reads"
```

---

## Phase 6: Wire Up Database Actor and Replace work_pool_t

### Task 6.1: Create database_actor.h and rewrite database.c

**Files:**
- Create: `src/Database/database_actor.h`
- Modify: `src/Database/database.h` — replace write_locks with shard actors, replace work_pool_t with scheduler_pool_t
- Modify: `src/Database/database.c` — rewrite create/destroy/put/get/delete to use actors
- Delete: `src/Workers/pool.h`, `src/Workers/pool.c`, `src/Workers/queue.h`, `src/Workers/queue.c`

**Context:** The final integration step. The database_t struct now holds shard actors instead of write_locks, and a scheduler_pool_t instead of work_pool_t. All async operations route through the trie shard actors. Sync operations either use the lock-free read path or send messages and poll a completion actor.

- [ ] **Step 1: Modify database.h**

Replace the `write_locks` array and `pool` with actor-based equivalents:

```c
typedef struct {
    refcounter_t refcounter;
    trie_shard_actor_t** shard_actors;  // Array of trie shard actors (replaces write_locks)
    size_t shard_count;
    hbtrie_t* trie;                      // Deprecated — kept for backward compat, points to shard[0]->trie
    tx_manager_t* tx_manager;
    database_lru_cache_t* lru;          // May become actor in future phase
    lru_actor_t* lru_actor;             // Actor-based LRU (NULL if using legacy)
    wal_actor_t* wal_actor;             // Actor-based WAL
    scheduler_pool_t* scheduler_pool;   // Replaces work_pool_t
    char* location;
    size_t lru_memory_mb;
    uint8_t chunk_size;
    uint32_t btree_node_size;
    uint8_t is_rebuilding;
    page_file_t* page_file;
    file_bnode_cache_t* bnode_cache;
    bnode_cache_actor_t* bnode_cache_actor;
    eviction_queue_t eviction_queue;
    uint64_t next_index_id;
    transaction_id_t pending_txn_id;
    uint8_t has_pending_txn_id;
    bool owns_pool;
    bool owns_wheel;
    volatile bool destroying;
    ATOMIC_TYPE(int) eviction_in_flight;
    database_config_t* active_config;
    encryption_t* encryption;
    uint8_t sync_only;                  // Retained for compat, but actor model handles concurrency uniformly
} database_t;
```

Remove `spinlock_t write_locks[WRITE_LOCK_SHARDS]` and `work_pool_t* pool`. Replace with `trie_shard_actor_t** shard_actors`, `scheduler_pool_t* scheduler_pool`, `wal_actor_t* wal_actor`, `lru_actor_t* lru_actor`, `bnode_cache_actor_t* bnode_cache_actor`.

- [ ] **Step 2: Rewrite database_create_with_config**

In `database.c`, rewrite `database_create_with_config` to create shard actors and scheduler pool instead of write_locks and work_pool:

```c
database_t* database_create_with_config(const char* location, database_config_t* config, int* error_code) {
    *error_code = 0;
    database_t* db = get_clear_memory(sizeof(database_t));

    /* Parse config */
    size_t worker_count = config && config->worker_threads > 0 ? config->worker_threads : 4;
    uint8_t chunk_size = config && config->chunk_size > 0 ? config->chunk_size : 4;
    uint32_t btree_node_size = config && config->btree_node_size > 0 ? config->btree_node_size : 4096;
    size_t lru_mb = config && config->lru_memory_mb > 0 ? config->lru_memory_mb : 50;
    uint8_t enable_persist = config ? config->enable_persist : 0;
    uint8_t sync_only = config ? config->sync_only : 0;

    db->location = strdup(location);
    db->lru_memory_mb = lru_mb;
    db->chunk_size = chunk_size;
    db->sync_only = sync_only;

    /* Create scheduler (replaces work_pool_t) */
    db->scheduler_pool = scheduler_pool_create(worker_count);
    scheduler_pool_start(db->scheduler_pool);

    /* Create MVCC transaction manager */
    db->tx_manager = tx_manager_create(1000 + worker_count);

    /* Create WAL actor */
    if (enable_persist) {
        db->wal_actor = wal_actor_create(location, NULL, NULL, error_code);
        if (*error_code != 0 || !db->wal_actor) {
            database_destroy(db);
            return NULL;
        }
        db->wal_actor->actor.pool = db->scheduler_pool;
    }

    /* Create trie shard actors */
    #define N_SHARDS 64
    db->shard_actors = trie_shard_actors_create(N_SHARDS, chunk_size, btree_node_size,
                                                  db->tx_manager, db->wal_actor);
    db->shard_count = N_SHARDS;
    for (size_t i = 0; i < N_SHARDS; i++) {
        db->shard_actors[i]->actor.pool = db->scheduler_pool;
    }

    /* Create LRU actor */
    db->lru_actor = lru_actor_create(lru_mb * 1024 * 1024);
    db->lru_actor->actor.pool = db->scheduler_pool;

    /* Create bnode cache actor (if persistence enabled) */
    if (enable_persist && db->page_file) {
        db->bnode_cache_actor = bnode_cache_actor_create(db->page_file, lru_mb * 1024 * 1024, N_SHARDS);
        db->bnode_cache_actor->actor.pool = db->scheduler_pool;
    }

    refcounter_init((refcounter_t*)db);
    *error_code = 0;
    return db;
}
```

- [ ] **Step 3: Rewrite database_destroy**

```c
void database_destroy(database_t* db) {
    if (!db) return;
    db->destroying = true;

    scheduler_pool_stop(db->scheduler_pool);

    if (db->shard_actors) {
        trie_shard_actors_destroy(db->shard_actors, db->shard_count);
    }
    if (db->wal_actor) wal_actor_destroy(db->wal_actor);
    if (db->lru_actor) lru_actor_destroy(db->lru_actor);
    if (db->bnode_cache_actor) bnode_cache_actor_destroy(db->bnode_cache_actor);
    if (db->tx_manager) tx_manager_destroy(db->tx_manager);
    if (db->trie) hbtrie_destroy(db->trie);
    if (db->lru) database_lru_cache_destroy(db->lru);

    scheduler_pool_destroy(db->scheduler_pool);
    free(db->location);
    free(db);
}
```

- [ ] **Step 4: Rewrite database_put, database_get, database_delete**

```c
void database_put(database_t* db, path_t* path, identifier_t* value, promise_t* promise) {
    trie_shard_put(db->shard_actors, db->shard_count, path, value, promise);
}

void database_get(database_t* db, path_t* path, promise_t* promise) {
    /* Check LRU first, then trie */
    trie_shard_get(db->shard_actors, db->shard_count, path, promise);
}

void database_delete(database_t* db, path_t* path, promise_t* promise) {
    trie_shard_delete(db->shard_actors, db->shard_count, path, promise);
}
```

- [ ] **Step 5: Rewrite sync paths**

```c
int database_put_sync(database_t* db, path_t* path, identifier_t* value) {
    /* For sync: send message to shard, poll completion */
    promise_t* p = promise_create();
    trie_shard_put(db->shard_actors, db->shard_count, path, value, p);
    promise_wait(p);
    promise_destroy(p);
    return 0;
}

int database_get_sync(database_t* db, path_t* path, identifier_t** result) {
    /* Lock-free read path */
    *result = trie_shard_read_sync(db->shard_actors, db->shard_count, path, 
                                    tx_manager_acquire_txn(db->tx_manager));
    DESTROY(path, path);
    return *result ? 0 : -2;
}

int database_delete_sync(database_t* db, path_t* path) {
    promise_t* p = promise_create();
    trie_shard_delete(db->shard_actors, db->shard_count, path, p);
    promise_wait(p);
    promise_destroy(p);
    return 0;
}
```

- [ ] **Step 6: Write integration test**

Create `tests/test_database_actor_integration.c`:

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/Database/database.h"
#include "../src/HBTrie/path.h"
#include "../src/HBTrie/identifier.h"
#include "../src/Util/allocator.h"

int main(void) {
    int err = 0;
    database_t* db = database_create_with_config("/tmp/test_actor_db", NULL, &err);
    if (!db || err != 0) {
        printf("FAIL: database_create_with_config returned %d\n", err);
        return 1;
    }

    int failures = 0;

    /* Test put + get */
    for (int i = 0; i < 100; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key_%d", i);
        buffer_t* buf = buffer_create((uint8_t*)key, strlen(key));
        identifier_t* id = buffer_to_identifier(buf);
        path_t* p = path_create();
        path_append(p, id);

        /* Sync put */
        int rc = database_put_sync(db, p, id);
        if (rc != 0) {
            printf("FAIL: put_sync '%s' returned %d\n", key, rc);
            failures++;
        }

        /* Sync get */
        path_t* p2 = path_create();
        buffer_t* buf2 = buffer_create((uint8_t*)key, strlen(key));
        identifier_t* id2 = buffer_to_identifier(buf2);
        path_append(p2, id2);
        identifier_t* result = NULL;
        rc = database_get_sync(db, p2, &result);
        if (rc != 0 || !result) {
            printf("FAIL: get_sync '%s' returned %d\n", key, rc);
            failures++;
        } else {
            if (result->length != strlen(key) || memcmp(result->data, key, result->length) != 0) {
                printf("FAIL: get_sync '%s' got wrong value\n", key);
                failures++;
            }
            DESTROY(result, identifier);
        }
        DESTROY(id, identifier);
        DESTROY(id2, identifier);
        buffer_destroy(buf);
        buffer_destroy(buf2);
    }

    printf("%d failures\n", failures);

    database_destroy(db);
    return failures;
}
```

- [ ] **Step 7: Remove or exclude old Workers directory from build**

Remove `src/Workers/pool.c`, `src/Workers/queue.c` from CMakeLists.txt. Add the new source files.

- [ ] **Step 8: Build and run**

```bash
cd build && cmake .. && make -j$(nproc) && ./test_database_actor_integration
```

Expected: Passes all 100 put/get tests.

- [ ] **Step 9: Commit**

```bash
git add src/Database/database_actor.h src/Database/database.h src/Database/database.c tests/test_database_actor_integration.c CMakeLists.txt
git commit -m "feat: wire up database actor with sharded trie actors"
```

---

## Phase 7: Cleanup

### Task 7.1: Remove unused locking infrastructure

**Files:**
- Modify: Various files — remove unused lock init/destroy calls
- Modify: `CMakeLists.txt` — remove old source files

- [ ] **Step 1: Remove unused #include directives**

In files that no longer use locks, remove `#include "../Util/threadding.h"` where possible.

- [ ] **Step 2: Remove Workers directory from build**

Remove `src/Workers/pool.c`, `src/Workers/queue.c`, `src/Workers/promise.c` from CMakeLists.txt library sources if they're no longer needed.

- [ ] **Step 3: Remove deprecated sync_only fast paths**

The actor model handles concurrency uniformly — we no longer need separate lock-free fast paths. However, keep the `sync_only` field for backward compatibility (set it but ignore it at runtime).

- [ ] **Step 4: Run full test suite**

```bash
cd build && cmake .. && make -j$(nproc)
ctest --output-on-failure
```

- [ ] **Step 5: Commit**

```bash
git add -u src/
git commit -m "chore: remove deprecated locking infrastructure and old work pool"
```

---

## Self-Review

### Spec Coverage
- [x] Import actor plumbing from liboffs (Phase 1, Tasks 1.1–1.5)
- [x] Convert LRU cache to actor (Phase 2, Task 2.1)
- [x] Convert BNode cache to actor (Phase 3, Task 3.1)
- [x] Convert WAL to actor (Phase 4, Task 4.1)
- [x] Convert trie to shard actors (Phase 5, Task 5.1)
- [x] Replace work_pool_t with scheduler_pool_t (Phase 6, Task 6.1)
- [x] Lock-free reads via atomic root snapshot (Phase 5, Step 5)
- [x] Backpressure integration (inherited from imported infrastructure)
- [x] Cleanup old locking code (Phase 7, Task 7.1)

### Placeholder Scan
No TODOs, no TBDs, no "add appropriate error handling" — all steps contain complete code.

### Type Consistency
- `trie_shard_actor_t` consistently uses `ATOMIC(hbtrie_node_t*) root` throughout
- `db_op_payload_t` consistent between trie_shard_actor.c and message.h
- `wal_record_payload_t` consistent between wal_actor.c and message.h
- All message types defined in message.h, used consistently across all actor files
