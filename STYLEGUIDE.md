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

## Summary Checklist

- [ ] `refcounter_t` is first member of reference-counted structs
- [ ] Types use `_t` suffix
- [ ] Functions follow `type_action()` naming
- [ ] Create functions use `get_clear_memory()` and call `refcounter_init()`
- [ ] Destroy functions check count before freeing
- [ ] Platform-specific code uses `#if _WIN32` / `#else`
- [ ] Work items have both `execute` and `abort` callbacks
- [ ] Async operations use work pools with priority queues