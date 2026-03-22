# WaveDB C Style Guide

## Struct Definitions

### Reference-Counted Objects
Structs that require reference counting **must** have `refcounter_t refcounter` as the **first member**. This allows safe pointer casting between the struct type and `refcounter_t*`.

```c
// Correct: refcounter is first member
typedef struct {
  refcounter_t refcounter;
  uint8_t* data;
  size_t size;
} buffer_t;

// Also correct: struct needs self-reference
typedef struct timer_list_node_t timer_list_node_t;
struct timer_list_node_t {
  timer_st* timer;
  timer_list_node_t* next;
  timer_list_node_t* previous;
};
```

### Naming Structs
- Use `typedef struct { ... } name_t;` for simple structs
- Use `typedef struct name_t name_t; struct name_t { ... };` for self-referential structs
- Always use the `_t` suffix for type names

## Naming Conventions

### Types
- All types use `lowercase_t` naming: `buffer_t`, `work_t`, `promise_t`, `work_pool_t`

### Functions
- Use `type_action()` pattern: `buffer_create`, `buffer_destroy`, `work_pool_enqueue`
- Create functions: `type_create()` returns a heap-allocated pointer
- Destroy functions: `type_destroy()` handles cleanup and deallocation
- Init functions: `type_init()` initializes embedded/stack-allocated structures (no allocation)

### Private Functions
- Internal functions can be declared at the top of the `.c` file or made `static`
- Use descriptive names without type prefix for internal helpers

### Macros
- Use `UPPER_CASE` for macros
- Platform-abstracted type macros: `PLATFORMLOCKTYPE(N)`, `PLATFORMTHREADTYPE`, `PLATFORMCONDITIONTYPE(N)`

## Create and Destroy Functions

### Create Pattern
```c
buffer_t* buffer_create(size_t size) {
  buffer_t* buf = get_clear_memory(sizeof(buffer_t));  // Zero-initialized memory
  buf->data = get_clear_memory(size);
  buf->size = size;
  refcounter_init((refcounter_t*) buf);  // Initialize refcounter last
  return buf;
}
```

**Rules:**
1. Use `get_clear_memory()` for allocation (zero-initialized, aborts on failure)
2. Use `get_memory()` when zero-initialization isn't needed
3. Cast to `refcounter_t*` when calling refcounter functions
4. Call `refcounter_init()` after all members are set

### Destroy Pattern
```c
void buffer_destroy(buffer_t* buf) {
  refcounter_dereference((refcounter_t*) buf);
  if (refcounter_count((refcounter_t*) buf) == 0) {
    free(buf->data);                      // Free internal resources first
    refcounter_destroy_lock(&buf->refcounter);  // Destroy lock before freeing struct
    free(buf);                            // Free the struct last
  }
}
```

**Rules:**
1. Always call `refcounter_dereference()` first
2. Check if `refcounter_count() == 0` before cleanup
3. Free internal resources first
4. Call `refcounter_destroy_lock()` before freeing the struct
5. Free the struct last

### Lock Initialization in Create Functions

**CRITICAL:** All locks and synchronization primitives MUST be initialized in create functions and destroyed in destroy functions.

```c
// CORRECT: Initialize all locks in create
mytype_t* mytype_create(void) {
  mytype_t* obj = get_clear_memory(sizeof(mytype_t));

  // Initialize all fields first
  obj->data = NULL;
  obj->count = 0;

  // Initialize ALL locks and condition variables
  platform_lock_init(&obj->mutex);
  platform_rw_lock_init(&obj->rwlock);
  platform_condition_init(&obj->cond);

  // Initialize refcounter LAST
  refcounter_init((refcounter_t*)obj);
  return obj;
}

// CORRECT: Destroy all locks in destroy
void mytype_destroy(mytype_t* obj) {
  refcounter_dereference((refcounter_t*)obj);
  if (refcounter_count((refcounter_t*)obj) == 0) {
    // Free internal resources
    free(obj->data);

    // Destroy ALL locks and condition variables
    platform_lock_destroy(&obj->mutex);
    platform_rw_lock_destroy(&obj->rwlock);
    platform_condition_destroy(&obj->cond);

    // Destroy refcounter lock and free struct
    refcounter_destroy_lock((refcounter_t*)obj);
    free(obj);
  }
}
```

**Why this matters:**
- `pthread_mutex_init()` MUST be called on memory before `pthread_mutex_lock()` can be used
- Memory from `get_clear_memory()` is zeroed, but a zeroed `pthread_mutex_t` is NOT a valid mutex
- Reused memory from freed objects may have stale lock state - always reinitialize
- Failing to initialize locks causes `EINVAL` (error 22) on lock attempts

**Common mistake:**
```c
// WRONG: Lock not initialized before use
mytype_t* mytype_create(void) {
  mytype_t* obj = get_clear_memory(sizeof(mytype_t));
  refcounter_init((refcounter_t*)obj);
  // MISSING: platform_lock_init(&obj->mutex);
  return obj;  // Lock is garbage!
}
```

## Reference Counting

### Core Functions
- `refcounter_init()` - Initialize counter to 1
- `refcounter_reference()` - Increment count, returns the pointer
- `refcounter_dereference()` - Decrement count
- `refcounter_count()` - Get current count
- `refcounter_yield()` - Mark for ownership transfer
- `refcounter_consume()` - Transfer ownership (yield + null pointer)

### Convenience Macros
```c
#define REFERENCE(N, T) (T*) refcounter_reference((refcounter_t*) N)
#define YIELD(N) refcounter_yield((refcounter_t*) N)
#define DEREFERENCE(N) refcounter_dereference((refcounter_t*) N); N = NULL
#define DESTROY(N, T) T##_destroy(N); N = NULL
#define CONSUME(N, T) (T*) refcounter_consume((refcounter_t**) &N)
```

### Usage Patterns

**Taking a reference (keep object alive):**
```c
work_t* work = (work_t*) refcounter_reference((refcounter_t*) work_item);
// ... use work ...
work_destroy(work);
```

**Yielding ownership (transfer to another owner):**
```c
refcounter_yield((refcounter_t*) work);  // Mark for handoff
work_pool_enqueue(pool, work);            // New owner takes over
```

**Consuming (take ownership from pointer):**
```c
work_t* mine = CONSUME(some_work, work);  // Takes ownership, nulls original
```

### Thread Safety
The refcounter supports both lock-based and atomic operations:
- Default: Uses platform mutex (`PLATFORMLOCKTYPE`)
- With `OFFS_ATOMIC` defined: Uses C11 atomics

## Asynchronous Code: Work Pools and Promises

### Work Pool Architecture
The work pool manages a pool of worker threads that execute work items from a priority queue.

```c
// Create and launch pool
work_pool_t* pool = work_pool_create(4);  // 4 workers
work_pool_launch(pool);

// Create and enqueue work
priority_t priority = {0};
work_t* work = work_create(priority, ctx, execute_callback, abort_callback);
work_pool_enqueue(pool, work);

// Shutdown
work_pool_shutdown(pool);
work_pool_wait_for_idle_signal(pool);
work_pool_join_all(pool);
work_pool_destroy(pool);
```

### Work Items
Work items encapsulate a unit of async work with execute and abort callbacks:

```c
typedef struct {
  refcounter_t refcounter;
  priority_t priority;
  void* ctx;
  void (*execute)(void*);
  void (*abort)(void*);
} work_t;
```

**Creating work:**
```c
void my_execute(void* ctx) {
  // Do the work
}

void my_abort(void* ctx) {
  // Handle cancellation/shutdown
}

work_t* work = work_create(priority, my_context, my_execute, my_abort);
```

### Promises
Promises provide resolve/reject semantics for async operations:

```c
typedef struct {
  refcounter_t refcounter;
  void (*resolve)(void*, void*);
  void (*reject)(void*, async_error_t*);
  void* ctx;
  uint8_t hasFired;
} promise_t;
```

**Usage:**
```c
void on_resolve(void* ctx, void* payload) {
  // Handle success
}

void on_reject(void* ctx, async_error_t* error) {
  // Handle failure
}

promise_t* promise = promise_create(on_resolve, on_reject, my_ctx);
```

### Priority System
Work items use a timestamp-based priority:

```c
typedef struct {
  uint64_t time;   // Timestamp
  uint64_t count;  // Sequence number for same-timestamp ordering
} priority_t;
```

Higher priority (lower timestamp) items execute first.

## Platform Abstraction

### Threading Primitives
Use the platform-agnostic macros for cross-platform compatibility:

```c
PLATFORMLOCKTYPE(my_lock);           // pthread_mutex_t or CRITICAL_SECTION
PLATFORMCONDITIONTYPE(my_cond);      // pthread_cond_t or CONDITION_VARIABLE
PLATFORMBARRIERTYPE(my_barrier);     // pthread_barrier_t or SYNCHRONIZATION_BARRIER
PLATFORMTHREADTYPE my_thread;        // pthread_t or HANDLE
```

### Platform-Specific Code
```c
#if _WIN32
  // Windows implementation
#else
  // POSIX implementation
#endif
```

## Memory Allocation

### Allocation Functions
```c
void* get_memory(size_t size);       // malloc wrapper, aborts on failure
void* get_clear_memory(size_t size); // calloc wrapper, aborts on failure
```

### Rules
1. **Prefer `get_clear_memory()`** - Zero-initialization prevents uninitialized bugs
2. Use `get_memory()` only when you will immediately overwrite all bytes
3. Always check return value for external allocations (not needed for these helpers)
4. These functions abort on allocation failure - don't check for NULL

## Error Handling

### Async Errors
```c
typedef struct {
  refcounter_t refcounter;
  char* message;
  char* file;
  char* function;
  int line;
} async_error_t;

// Create with automatic location info
async_error_t* err = ERROR("Something went wrong");
```

### Guard Clauses
```c
void some_function(buffer_t* buf) {
  if (buf == NULL) return;  // Early return for invalid input
  // ... rest of function
}
```

## Code Organization

### Source Directory Structure
The `src/` folder is organized into subdirectories that group files by their **semantic purpose**. Each directory represents a distinct domain or responsibility, keeping related code together.

```
src/
├── Buffer/           // Binary data manipulation (buffer.c, buffer.h)
├── RefCounter/       // Reference counting infrastructure (refcounter.c, refcounter.h)
├── Time/             // Timing and scheduling (wheel.c, ticker.c, debouncer.c)
├── Util/             // Cross-cutting utilities (allocator.c, log.c, vec.c, hash.c, threading.c)
└── Workers/          // Async execution primitives (pool.c, work.c, promise.c, queue.c, priority.c, error.c)
```

**Directory Organization Principles:**
- **Semantic grouping**: Files are organized by what purpose they serve, not by file type
- **Each directory = one concern**: `Workers/` handles all async execution, `Time/` handles all timing-related code
- **PascalCase naming**: Directory names use PascalCase (e.g., `RefCounter`, `Workers`)
- **Co-located headers**: `.h` and `.c` files live in the same directory

**When adding new code:**
- Place new files in the directory matching their semantic purpose
- If no existing directory fits, create a new directory with a descriptive PascalCase name
- Keep related functionality together rather than spreading across directories

### Module File Structure
```
Module/
├── module.h      // Public interface
└── module.c      // Implementation
```

### Header File Template
```c
//
// Created by victor on MM/DD/YY.
//

#ifndef MODULE_NAME_H
#define MODULE_NAME_H

#include <stdint.h>
#include "../RefCounter/refcounter.h"

typedef struct {
  refcounter_t refcounter;
  // members
} module_name_t;

module_name_t* module_name_create(/* params */);
void module_name_destroy(module_name_t* obj);
// other public functions

#endif // MODULE_NAME_H
```

### Include Order
1. Module's own header first
2. Project headers (relative paths)
3. Standard library headers
4. External library headers

## Work Pool and Timing Wheel Lifecycle

### Proper Initialization Order
The work pool and timing wheel must be initialized in a specific order, and shutdown must follow the reverse order.

**Correct Startup Sequence:**
```c
// 1. Create work pool first (use platform_core_count() for thread count)
work_pool_t* pool = work_pool_create(platform_core_count());
work_pool_launch(pool);  // Start worker threads

// 2. Create timing wheel AFTER pool (wheel depends on pool for timer callbacks)
hierarchical_timing_wheel_t* wheel = hierarchical_timing_wheel_create(8, pool);
hierarchical_timing_wheel_run(wheel);  // Start the timing thread
```

**Correct Shutdown Sequence:**
```c
// 1. Stop the timing wheel FIRST (wait for pending timers)
hierarchical_timing_wheel_wait_for_idle_signal(wheel);  // Block until timers complete
hierarchical_timing_wheel_stop(wheel);                   // Signal timing thread to stop

// 2. Shutdown work pool AFTER timing wheel is stopped
work_pool_shutdown(pool);    // Signal workers to stop accepting work
work_pool_join_all(pool);    // Wait for all workers to finish

// 3. Destroy in reverse order of creation
work_pool_destroy(pool);
hierarchical_timing_wheel_destroy(wheel);
```

### Why This Order Matters

1. **Timing wheel depends on work pool**: The timing wheel uses the work pool to execute timer callbacks. If you destroy the pool first, the wheel can't execute its callbacks.

2. **Wait before stop**: `hierarchical_timing_wheel_wait_for_idle_signal()` ensures all scheduled timers have fired before proceeding. This prevents callbacks from running after resources they depend on are destroyed.

3. **Shutdown then join**: `work_pool_shutdown()` signals workers to stop. `work_pool_join_all()` waits for them to finish their current work. This two-step process allows graceful shutdown.

### Common Mistakes

**Wrong: Destroying pool before wheel**
```c
// DANGEROUS: Wheel callbacks may still be queued in pool
work_pool_destroy(pool);
hierarchical_timing_wheel_stop(wheel);  // Callbacks could crash
```

**Wrong: Not waiting for idle**
```c
// DANGEROUS: Timers may still be pending
hierarchical_timing_wheel_stop(wheel);  // Timers interrupted mid-execution
```

**Wrong: Hardcoded thread count**
```c
// BAD: May not match system capabilities
work_pool_t* pool = work_pool_create(4);

// GOOD: Use actual core count
work_pool_t* pool = work_pool_create(platform_core_count());
```

### Testing Async Code

When writing tests that use work pools and timing wheels, follow this pattern:

```c
class MyTest : public ::testing::Test {
protected:
    void SetUp() override {
        pool = work_pool_create(platform_core_count());
        work_pool_launch(pool);
        wheel = hierarchical_timing_wheel_create(8, pool);
        hierarchical_timing_wheel_run(wheel);
    }

    void TearDown() override {
        // Destroy dependent objects first (database, etc.)
        if (my_object) {
            my_object_destroy(my_object);
            my_object = nullptr;
        }

        // Stop timing wheel (wait for idle, then stop)
        if (wheel) {
            hierarchical_timing_wheel_wait_for_idle_signal(wheel);
            hierarchical_timing_wheel_stop(wheel);
        }

        // Shutdown and join pool
        if (pool) {
            work_pool_shutdown(pool);
            work_pool_join_all(pool);
        }

        // Destroy pool and wheel
        if (pool) {
            work_pool_destroy(pool);
            pool = nullptr;
        }
        if (wheel) {
            hierarchical_timing_wheel_destroy(wheel);
            wheel = nullptr;
        }
    }

    work_pool_t* pool = nullptr;
    hierarchical_timing_wheel_t* wheel = nullptr;
};
```

## Debouncer Lifecycle

### Always Flush Debouncers Before Destroying

**CRITICAL RULE:** When destroying objects that hold debouncers, you **MUST** call `debouncer_flush()` before `debouncer_destroy()`. The debouncer schedules asynchronous operations on the timing wheel, and if you destroy it without flushing, those operations will still be queued and may try to access destroyed resources.

**Correct Pattern:**
```c
void my_object_destroy(my_object_t* obj) {
    if (obj == NULL) return;

    refcounter_dereference((refcounter_t*) obj);
    if (refcounter_count((refcounter_t*) obj) == 0) {
        // Flush debouncer BEFORE destroying
        if (obj->debouncer != NULL) {
            debouncer_flush(obj->debouncer);     // Execute pending operations synchronously
            debouncer_destroy(obj->debouncer);   // Now safe to destroy
            obj->debouncer = NULL;
        }

        // Destroy other resources
        free(obj->data);
        refcounter_destroy_lock(&obj->refcounter);
        free(obj);
    }
}
```

**Why This Matters:**

1. **Debouncers hold timing wheel references**: A debouncer schedules callbacks to run later on the timing wheel
2. **Timing wheel executes asynchronously**: Even after you call `my_object_destroy()`, the timing wheel still has pending work items
3. **Use-after-free risk**: Those work items hold pointers to your object, which is now destroyed
4. **Race conditions**: Sequential test runs fail because pending callbacks from test N crash during test N+1

**Real Bug Example:**
```c
// WRONG: Debouncer destroyed without flushing
void wal_manager_destroy(wal_manager_t* manager) {
    if (twal->fsync_debouncer != NULL) {
        debouncer_destroy(twal->fsync_debouncer);  // Bug: Pending fsync still queued!
    }
    // Result: Timing wheel tries to call fsync callback on destroyed WAL
}

// CORRECT: Flush first, then destroy
void wal_manager_destroy(wal_manager_t* manager) {
    if (twal->fsync_debouncer != NULL) {
        debouncer_flush(twal->fsync_debouncer);    // Execute pending fsync now
        debouncer_destroy(twal->fsync_debouncer);  // Safe to destroy
    }
}
```

**When to Flush:**

- **Object destruction**: Always flush before destroying objects with debouncers (WAL, database, etc.)
- **Before resource cleanup**: If your object has pending operations, flush them before freeing dependent resources
- **Test teardown**: Flush debouncers in TearDown() before destroying timing wheels or work pools

**Debouncer Flush Guarantees:**

- `debouncer_flush()` executes pending callbacks **synchronously**
- All queued operations complete before `debouncer_flush()` returns
- After flush, no more callbacks will fire from this debouncer
- Safe to call on NULL (returns immediately)
- Idempotent (calling flush multiple times is safe)

### Testing with Debouncers

When testing code that uses debouncers with timing wheels:

```c
class MyTest : public ::testing::Test {
protected:
    void TearDown() override {
        // Destroy objects with debouncers FIRST (they'll flush internally)
        if (database) {
            database_destroy(database);  // Internally flushes debouncers
            database = nullptr;
        }

        // Now safe to stop timing wheel
        if (wheel) {
            hierarchical_timing_wheel_wait_for_idle_signal(wheel);
            hierarchical_timing_wheel_stop(wheel);
            hierarchical_timing_wheel_destroy(wheel);
            wheel = nullptr;
        }

        // Finally, destroy work pool
        if (pool) {
            work_pool_shutdown(pool);
            work_pool_join_all(pool);
            work_pool_destroy(pool);
            pool = nullptr;
        }
    }
};
```

**Key Insight:** The destruction order matters. Objects with debouncers must be destroyed **before** stopping the timing wheel, because they flush their debouncers during destruction. If you stop the wheel first, flush can't execute the pending operations.

## Batch Write API

### Creating a Batch

Batches allow grouping multiple write operations for efficient submission:

```c
// Create batch with expected capacity
batch_t* batch = batch_create(1000);

// Add operations
for (int i = 0; i < 1000; i++) {
    path_t* path = generate_path(i);
    identifier_t* value = generate_value(i);
    batch_add_put(batch, path, value);
}

// Submit synchronously
int result = database_write_batch_sync(db, batch);

// Check result
if (result != 0) {
    // Handle error
}

// Destroy batch
batch_destroy(batch);
```

### Error Handling

Batch operations return error codes. Handle full batches and submission errors:

```c
int result = batch_add_put(batch, path, value);
if (result == -2) {
    // Batch is full - ownership stays with caller
    path_destroy(path);
    identifier_destroy(value);
} else if (result == -6) {
    // Batch already submitted
}
```

### Async Batch Submission

For non-blocking writes, use async submission with promises:

```c
// Create promise
promise_t* promise = promise_create();

// Submit async
database_write_batch(db, batch, promise);

// Wait for result
promise_wait(promise);
int result;
promise_get_result(promise, &result, sizeof(int));

// Cleanup
promise_destroy(promise);
batch_destroy(batch);
```

## Summary Checklist

- [ ] `refcounter_t` is first member of reference-counted structs
- [ ] Types use `_t` suffix
- [ ] Functions follow `type_action()` naming
- [ ] Create functions use `get_clear_memory()` and call `refcounter_init()`
- [ ] Destroy functions check count before freeing
- [ ] Platform-specific code uses `#if _WIN32` / `#else`
- [ ] Work items have both `execute` and `abort` callbacks
- [ ] Async operations use work pools with priority queues
- [ ] Work pool created before timing wheel, destroyed after
- [ ] `hierarchical_timing_wheel_wait_for_idle_signal()` called before `stop()`
- [ ] `work_pool_shutdown()` + `work_pool_join_all()` before `work_pool_destroy()`
- [ ] Use `platform_core_count()` for pool size, not hardcoded values
- [ ] **Debouncers: ALWAYS call `debouncer_flush()` before `debouncer_destroy()`**

## Work Pool and Timing Wheel Lifecycle

### Initialization Order
When setting up async infrastructure, always follow this order:

```c
// 1. Create work pool first
work_pool_t* pool = work_pool_create(platform_core_count());

// 2. Launch the pool to start worker threads
work_pool_launch(pool);

// 3. Create timing wheel with the pool
hierarchical_timing_wheel_t* wheel = hierarchical_timing_wheel_create(8, pool);

// 4. Start the timing wheel
hierarchical_timing_wheel_run(wheel);

// 5. Now create components that depend on these (databases, debouncers, etc.)
database_t* db = database_create(path, 0, 0, 0, 0, pool, wheel, &error);
```

### Shutdown Order (CRITICAL)
Shutdown **must** follow this exact order to avoid deadlocks:

```c
// 1. Destroy dependent components first (databases, etc.)
database_destroy(db);
db = NULL;

// 2. Wait for timing wheel to complete pending work BEFORE stopping
if (wheel) {
    hierarchical_timing_wheel_wait_for_idle_signal(wheel);
    hierarchical_timing_wheel_stop(wheel);
}

// 3. Shutdown the work pool (stops accepting new work)
if (pool) {
    work_pool_shutdown(pool);
    work_pool_join_all(pool);  // Wait for threads to finish
}

// 4. Destroy resources in reverse order of creation
if (pool) {
    work_pool_destroy(pool);
    pool = NULL;
}
if (wheel) {
    hierarchical_timing_wheel_destroy(wheel);
    wheel = NULL;
}
```

**Why this order matters:**
1. Timing wheel schedules work on the pool - stopping it first prevents new work
2. Work pool threads may be executing callbacks that use the timing wheel
3. Waiting for idle signal ensures all timers have fired before shutdown
4. Joining threads after shutdown ensures clean termination

### Testing Async Operations with Promises

When testing async operations that use work pools and timing wheels, use `std::promise` and `std::future` for synchronization:

```cpp
// 1. Declare promise arrays as test class members (not static!)
class MyTest : public ::testing::Test {
protected:
    std::promise<void> put_promise[MAX_COUNT];
    std::promise<result_t*> get_promise[MAX_COUNT];
    // ... test fixtures
};

// 2. Create a context struct with test pointer and index
typedef struct {
    size_t i;
    MyTest* test;
} test_ctx;

// 3. Create wrapper callbacks that set promises
extern "C" void callback_wrapper(void* ctx, void* payload) {
    auto tc = static_cast<test_ctx*>(ctx);
    tc->test->get_promise[tc->i].set_value((result_t*)payload);
    free(ctx);
}

extern "C" void error_callback_wrapper(void* ctx, async_error_t* error) {
    auto tc = static_cast<test_ctx*>(ctx);
    try {
        throw std::runtime_error((const char*)error->message);
    } catch(...) {
        tc->test->get_promise[tc->i].set_exception(std::current_exception());
    }
    error_destroy(error);
    free(ctx);
}

// 4. In tests, create context and wait on future
TEST_F(MyTest, AsyncOperation) {
    test_ctx* ctx = (test_ctx*)get_memory(sizeof(test_ctx));
    ctx->i = 0;
    ctx->test = this;

    promise_t* promise = promise_create(callback_wrapper, error_callback_wrapper, ctx);
    async_operation(obj, priority, promise);

    std::future<result_t*> future = get_promise[0].get_future();
    result_t* result = nullptr;
    EXPECT_NO_THROW({ result = future.get(); });

    // Verify result...
    promise_destroy(promise);
}
```

**Key points:**
- Promise arrays are member variables (fresh per test), not static globals
- Context includes test pointer for accessing member promises
- Callbacks must free their context
- Use `extern "C"` for callbacks called from C code
- `get_memory()` is used for context allocation (matches C allocation pattern)

## Debouncer Lifecycle

### Immediate Flush on Destroy

Objects that own debouncers **must** flush and destroy them immediately in their destroy function, before freeing any other resources. The debouncer callback runs synchronously during flush, so it must be called while the object is still in a valid state.

**Correct Pattern:**
```c
void my_object_destroy(my_object_t* obj) {
    if (obj == NULL) return;

    refcounter_dereference((refcounter_t*)obj);
    if (refcounter_count((refcounter_t*)obj) == 0) {
        // Prevent new callbacks from being scheduled
        platform_lock(&obj->callback_lock);
        obj->destroy_requested = 1;
        // Wait for any in-progress callback to complete
        while (obj->callback_in_progress) {
            platform_condition_wait(&obj->callback_lock, &obj->callback_done);
        }
        platform_unlock(&obj->callback_lock);

        // CRITICAL: Flush and destroy debouncer FIRST
        // The callback runs synchronously and accesses object state
        if (obj->debouncer) {
            debouncer_flush(obj->debouncer);    // Runs callback synchronously
            debouncer_destroy(obj->debouncer);   // Frees debouncer
            obj->debouncer = NULL;
        }

        // NOW safe to free other resources
        if (obj->data) free(obj->data);
        if (obj->buffer) buffer_destroy(obj->buffer);

        // Destroy locks and free struct
        refcounter_destroy_lock((refcounter_t*)obj);
        free(obj);
    }
}
```

### Why This Matters

1. **Callback runs synchronously**: `debouncer_flush()` calls the callback in the current thread, blocking until it completes
2. **Callback needs valid object**: The callback accesses object state (e.g., `db->write_trie`), so the object must not be partially freed
3. **Prevents use-after-free**: Flushing after freeing resources would cause the callback to access freed memory
4. **Timer cancellation**: `debouncer_flush()` cancels any pending timer in the timing wheel, preventing later callbacks

### Common Mistakes

**Wrong: Free resources before flushing debouncer**
```c
// DANGEROUS: Callback will access freed memory
if (obj->data) free(obj->data);
if (obj->debouncer) {
    debouncer_flush(obj->debouncer);  // Callback accesses freed obj->data!
    debouncer_destroy(obj->debouncer);
}
```

**Wrong: Don't flush at all**
```c
// DANGEROUS: Timer may fire after object is freed
if (obj->debouncer) {
    debouncer_destroy(obj->debouncer);  // Timer still scheduled in wheel!
}
free(obj);  // Timer callback will use freed pointer
```

**Correct: Flush and destroy debouncer first**
```c
// SAFE: Flush runs callback while object is valid, then destroy
if (obj->debouncer) {
    debouncer_flush(obj->debouncer);
    debouncer_destroy(obj->debouncer);
    obj->debouncer = NULL;
}
// Now safe to free other resources
if (obj->data) free(obj->data);
```

### Debouncer Callback Pattern

The debouncer callback must check if destruction is in progress:

```c
static void my_object_debouncer_callback(void* ctx) {
    my_object_t* obj = (my_object_t*)ctx;

    // Check if object is being destroyed
    platform_lock(&obj->callback_lock);
    if (obj->destroy_requested) {
        platform_unlock(&obj->callback_lock);
        return;  // Don't do anything, object is shutting down
    }
    obj->callback_in_progress = 1;
    platform_unlock(&obj->callback_lock);

    // Do the work (object is guaranteed to stay alive during callback)
    // ... perform snapshot, flush, etc. ...

    // Signal completion
    platform_lock(&obj->callback_lock);
    obj->callback_in_progress = 0;
    platform_signal_condition(&obj->callback_done);
    platform_unlock(&obj->callback_lock);
}
```

### Key Points

- Flush happens **before** any other resource cleanup
- Callback must check `destroy_requested` flag
- Use `callback_in_progress` + condition variable to wait for callback completion
- `debouncer_flush()` is synchronous - it blocks until the callback completes
- After `debouncer_destroy()`, the debouncer pointer should be set to NULL

## Debouncer Lifecycle Management

### Critical: Flush Debouncers Before Destroy

Objects that own debouncers **must** flush and destroy them immediately in their destroy function. Debouncers schedule callbacks via timing wheels, and those callbacks reference the owning object. If the object is destroyed while a callback is scheduled, it results in a use-after-free crash.

**Correct Pattern:**
```c
void my_object_destroy(my_object_t* obj) {
    if (obj == NULL) return;

    refcounter_dereference((refcounter_t*)obj);
    if (refcounter_count((refcounter_t*)obj) == 0) {
        // 1. Signal that destruction is starting (prevent new callbacks)
        platform_lock(&obj->lock);
        obj->destroy_requested = 1;
        // Wait for any in-progress callback to complete
        while (obj->callback_in_progress) {
            platform_condition_wait(&obj->lock, &obj->callback_done);
        }
        platform_unlock(&obj->lock);

        // 2. Flush and destroy debouncer IMMEDIATELY
        if (obj->debouncer) {
            debouncer_flush(obj->debouncer);    // Synchronously executes pending callback
            debouncer_destroy(obj->debouncer);   // Frees debouncer memory
            obj->debouncer = NULL;
        }

        // 3. Now safe to free other resources
        if (obj->other_resource) {
            other_resource_destroy(obj->other_resource);
        }

        // 4. Destroy locks and free object
        platform_lock_destroy(&obj->lock);
        platform_condition_destroy(&obj->callback_done);
        refcounter_destroy_lock((refcounter_t*)obj);
        free(obj);
    }
}
```

**Callback guard pattern:**
```c
static void my_object_callback(void* ctx) {
    my_object_t* obj = (my_object_t*)ctx;

    // Check if destroy is in progress
    platform_lock(&obj->lock);
    if (obj->destroy_requested) {
        platform_unlock(&obj->lock);
        return;  // Object is being destroyed, don't proceed
    }
    obj->callback_in_progress = 1;
    platform_unlock(&obj->lock);

    // ... perform work ...

    // Signal completion
    platform_lock(&obj->lock);
    obj->callback_in_progress = 0;
    platform_signal_condition(&obj->callback_done);
    platform_unlock(&obj->lock);
}
```

### Why This Matters

1. **Timing wheel callbacks reference the object**: The debouncer's callback receives the object as `ctx`. If the object is freed before the callback runs, dereferencing `ctx` causes a crash.

2. **`debouncer_flush()` is synchronous**: It cancels pending timers and calls the callback directly in the current thread, ensuring the callback completes before returning.

3. **Race condition prevention**: The `destroy_requested` flag + `callback_in_progress` counter ensure that:
   - New callbacks don't start after destroy begins
   - Destroy waits for in-progress callbacks to complete
   - Flush executes any remaining callbacks synchronously

### Common Mistakes

**Wrong: Destroying without flushing**
```c
// DANGEROUS: Callback may still be scheduled
void my_object_destroy(my_object_t* obj) {
    if (obj->debouncer) {
        debouncer_destroy(obj->debouncer);  // Timer still scheduled!
    }
    free(obj);  // Callback may run after this - use-after-free!
}
```

**Wrong: Flushing without guard**
```c
// DANGEROUS: Callback may start during destroy
void my_object_destroy(my_object_t* obj) {
    if (obj->debouncer) {
        debouncer_flush(obj->debouncer);  // Callback runs, accesses obj
        // But obj->data might already be freed!
    }
    free(obj->data);  // Callback may have accessed this
    free(obj);
}
```

**Correct: Guard + flush + destroy**
```c
void my_object_destroy(my_object_t* obj) {
    // Lock and set flag first
    platform_lock(&obj->lock);
    obj->destroy_requested = 1;
    while (obj->callback_in_progress) {
        platform_condition_wait(&obj->lock, &obj->callback_done);
    }
    platform_unlock(&obj->lock);

    // Now flush (callback won't run due to guard)
    if (obj->debouncer) {
        debouncer_flush(obj->debouncer);
        debouncer_destroy(obj->debouncer);
    }

    // Safe to free resources
    free(obj->data);
    free(obj);
}
```