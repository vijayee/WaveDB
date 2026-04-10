# Timing Wheel Race Condition Fix

## Bug

Heap-use-after-free in the hierarchical timing wheel's shared `timer_map_t` hashmap, triggered by concurrent access from two code paths holding different locks.

## Root Cause

The `hierarchical_timing_wheel_t` owns a `timer_map_t timers` hashmap and a `PLATFORMLOCKTYPE(lock)`. Its sub-wheels (`milliseconds`, `seconds`, `minutes`, `hours`, `days`) each hold a `timer_map_t* timers` pointer to that same shared hashmap, plus their own `PLATFORMLOCKTYPE(lock)`.

Two code paths accessed the shared hashmap under **different mutexes**:

1. **Sub-wheel tick path** (`timing_wheel_on_tick` -> `timing_wheel_maintenance`): Holds the sub-wheel's `&wheel->lock`, calls `hashmap_remove(wheel->timers, ...)` and `hashmap_size(wheel->timers)` under that sub-wheel lock only.

2. **Hierarchical wheel API path** (`hierarchical_timing_wheel_cancel_timer`, `hierarchical_timing_wheel_set_timer`): Holds the hierarchical wheel's `&wheel->lock`, calls `hashmap_remove`, `hashmap_put`, `hashmap_get`, and `hashmap_size` on the same hashmap.

The hashmap uses `duplicate_uint64` / `free` as key allocators. When `cancel_timer` frees a duplicated key via `hashmap_remove` under the hierarchical lock, and `maintenance` simultaneously reads or modifies the hashmap under only the sub-wheel lock, the freed key can be accessed — causing heap-use-after-free.

Symptom under AddressSanitizer:

```
==ERROR: AddressSanitizer: heap-use-after-free on address 0x...
    #0 ... in compare_uint64
    #1 ... in hashmap_base_put
    #2 ... in hierarchical_timing_wheel_set_timer
```

The `debouncer_debounce` function triggers this race by calling `cancel_timer` then immediately `set_timer`, which exercises both paths in quick succession.

## Fix

Give each sub-wheel a pointer to the hierarchical wheel's lock, and acquire that lock whenever accessing the shared hashmap from the sub-wheel side.

### Why this is safe (no deadlock)

The lock ordering is consistent:

| Path | Lock order |
|------|-----------|
| Sub-wheel tick | sub-wheel lock -> hierarchical lock (nested in `maintenance`) |
| `set_timer` | hierarchical lock (release) -> sub-wheel lock (sequential, not nested) |
| `cancel_timer` | hierarchical lock only |

`hierarchical_timing_wheel_set_timer` releases the hierarchical lock **before** calling `timing_wheel_set_timer` (which acquires the sub-wheel lock), so there is no ABBA deadlock.

## Changes

### 1. `src/Util/threadding.h` — Add `PLATFORMLOCKTYPEPTR` macro

The codebase already has `PLATFORMCONDITIONTYPEPTR` for condition variable pointers but was missing the equivalent for mutex pointers. Add it alongside the existing `PLATFORMLOCKTYPE`:

**Linux (pthread section):**
```c
#define PLATFORMLOCKTYPEPTR(N) pthread_mutex_t* N
```

**Windows (CRITICAL_SECTION section):**
```c
#define PLATFORMLOCKTYPEPTR(N) CRITICAL_SECTION* N
```

### 2. `src/Time/wheel.h` — Add `hierarchical_lock` field to `timing_wheel_t`

Add a pointer field that will point back to the hierarchical wheel's lock:

```c
struct timing_wheel_t {
  refcounter_t refcounter;
  PLATFORMLOCKTYPE(lock);
  PLATFORMLOCKTYPEPTR(hierarchical_lock);  // <-- NEW: points to hierarchical wheel's lock
  PLATFORMCONDITIONTYPEPTR(idle);
  size_t position;
  timer_map_t* timers;
  // ... rest unchanged
};
```

Update the `timing_wheel_create` declaration to accept the new parameter:

```c
timing_wheel_t* timing_wheel_create(uint64_t interval, size_t slot_count, work_pool_t* pool, timer_map_t* timers, PLATFORMLOCKTYPEPTR(hierarchical_lock), PLATFORMCONDITIONTYPEPTR(idle));
```

### 3. `src/Time/wheel.c` — Three changes

**3a. `timing_wheel_create` — Store the lock pointer**

Add the `hierarchical_lock` parameter and store it:

```c
timing_wheel_t* timing_wheel_create(uint64_t interval, size_t slot_count, work_pool_t* pool, timer_map_t* timers, PLATFORMLOCKTYPEPTR(hierarchical_lock), PLATFORMCONDITIONTYPEPTR(idle)) {
  // ... existing init ...
  wheel->timers = timers;
  wheel->hierarchical_lock = hierarchical_lock;  // <-- NEW
  wheel->idle = idle;
  // ... rest unchanged ...
}
```

**3b. `timing_wheel_maintenance` — Protect `hashmap_remove` with hierarchical lock**

In the while loop, wrap each `hashmap_remove` call:

```c
    timer_list_enqueue(expired, timer);
    next = current->next;
    platform_lock(wheel->hierarchical_lock);       // <-- NEW
    hashmap_remove(wheel->timers, &timer->timerId);
    platform_unlock(wheel->hierarchical_lock);     // <-- NEW
    timer_list_remove(list, current);
```

**3c. `timing_wheel_fire_expired` — Protect `hashmap_size` check with hierarchical lock**

Replace the unguarded check:

```c
  // BEFORE:
  if (hashmap_size(wheel->timers) == 0) {
    platform_signal_condition(wheel->idle);
  }
```

With:

```c
  // AFTER:
  platform_lock(wheel->hierarchical_lock);
  if (hashmap_size(wheel->timers) == 0) {
    platform_unlock(wheel->hierarchical_lock);
    platform_signal_condition(wheel->idle);
  } else {
    platform_unlock(wheel->hierarchical_lock);
  }
```

**3d. `hierarchical_timing_wheel_create` — Pass `&wheel->lock` to each sub-wheel**

Each sub-wheel creation call gets a new `&wheel->lock` argument between the `timers` and `idle` parameters:

```c
  wheel->milliseconds = timing_wheel_create(1, slot_count, pool, &wheel->timers, &wheel->lock, &wheel->idle);
  wheel->seconds = timing_wheel_create(Time_Seconds, slot_count, pool, &wheel->timers, &wheel->lock, &wheel->idle);
  wheel->minutes = timing_wheel_create(Time_Minutes, slot_count, pool, &wheel->timers, &wheel->lock, &wheel->idle);
  wheel->hours = timing_wheel_create(Time_Hours, slot_count, pool, &wheel->timers, &wheel->lock, &wheel->idle);
  wheel->days = timing_wheel_create(Time_Days, slot_count, pool, &wheel->timers, &wheel->lock, &wheel->idle);
```

## Full Diff

```diff
diff --git a/src/Time/wheel.c b/src/Time/wheel.c
index 52edf6a..c9ed8cb 100644
--- a/src/Time/wheel.c
+++ b/src/Time/wheel.c
@@ -120,7 +120,7 @@ timer_st* timer_list_remove(timer_list_t* list, timer_list_node_t* node) {
   return timer;
 }
 
-timing_wheel_t* timing_wheel_create(uint64_t interval, size_t slot_count, work_pool_t* pool, timer_map_t* timers, PLATFORMCONDITIONTYPEPTR(idle)) {
+timing_wheel_t* timing_wheel_create(uint64_t interval, size_t slot_count, work_pool_t* pool, timer_map_t* timers, PLATFORMLOCKTYPEPTR(hierarchical_lock), PLATFORMCONDITIONTYPEPTR(idle)) {
   timing_wheel_t* wheel = get_clear_memory(sizeof(timing_wheel_t));
   refcounter_init((refcounter_t*) wheel);
   platform_lock_init(&wheel->lock);
@@ -129,6 +129,7 @@ timing_wheel_t* timing_wheel_create(uint64_t interval, size_t slot_count, work_p
   wheel->position = slot_count - 1;
   wheel->slots = get_clear_memory(sizeof(slots_t));
   wheel->timers = timers;
+  wheel->hierarchical_lock = hierarchical_lock;
   wheel->idle = idle;
   vec_init(wheel->slots);
   vec_reserve(wheel->slots, slot_count);
@@ -214,8 +215,12 @@ void timing_wheel_fire_expired(timing_wheel_t* wheel, timer_list_t* expired) {
     current = timer_list_dequeue(expired);
 
   }
+  platform_lock(wheel->hierarchical_lock);
   if (hashmap_size(wheel->timers) == 0) {
+    platform_unlock(wheel->hierarchical_lock);
     platform_signal_condition(wheel->idle);
+  } else {
+    platform_unlock(wheel->hierarchical_lock);
   }
   timer_list_destroy(expired);
 }
@@ -246,7 +251,9 @@ timer_list_t* timing_wheel_maintenance(timing_wheel_t* wheel) {
     }
     timer_list_enqueue(expired, timer);
     next = current->next;
+    platform_lock(wheel->hierarchical_lock);
     hashmap_remove(wheel->timers, &timer->timerId);
+    platform_unlock(wheel->hierarchical_lock);
     timer_list_remove(list, current);
     current = next;
   }
@@ -387,11 +394,11 @@ hierarchical_timing_wheel_t* hierarchical_timing_wheel_create(size_t slot_count,
   wheel->next_id = 1;
   hashmap_init(&wheel->timers, (void*)hash_uint64, (void*)compare_uint64);
   hashmap_set_key_alloc_funcs(&wheel->timers, duplicate_uint64, (void*)free);
-  wheel->milliseconds = timing_wheel_create(1, slot_count, pool, &wheel->timers, &wheel->idle);
-  wheel->seconds = timing_wheel_create(Time_Seconds, slot_count, pool, &wheel->timers, &wheel->idle);
-  wheel->minutes = timing_wheel_create(Time_Minutes, slot_count, pool, &wheel->timers, &wheel->idle);
-  wheel->hours = timing_wheel_create(Time_Hours, slot_count, pool, &wheel->timers, &wheel->idle);
-  wheel->days = timing_wheel_create(Time_Days, slot_count, pool, &wheel->timers, &wheel->idle);
+  wheel->milliseconds = timing_wheel_create(1, slot_count, pool, &wheel->timers, &wheel->lock, &wheel->idle);
+  wheel->seconds = timing_wheel_create(Time_Seconds, slot_count, pool, &wheel->timers, &wheel->lock, &wheel->idle);
+  wheel->minutes = timing_wheel_create(Time_Minutes, slot_count, pool, &wheel->timers, &wheel->lock, &wheel->idle);
+  wheel->hours = timing_wheel_create(Time_Hours, slot_count, pool, &wheel->timers, &wheel->lock, &wheel->idle);
+  wheel->days = timing_wheel_create(Time_Days, slot_count, pool, &wheel->timers, &wheel->lock, &wheel->idle);
 
   wheel->seconds->wheel = wheel->milliseconds;
   wheel->minutes->wheel = wheel->seconds;
diff --git a/src/Time/wheel.h b/src/Time/wheel.h
index 438f9a5..57d8f25 100644
--- a/src/Time/wheel.h
+++ b/src/Time/wheel.h
@@ -82,6 +82,7 @@ typedef vec_t(timer_list_t*) slots_t;
 struct timing_wheel_t {
   refcounter_t refcounter;
   PLATFORMLOCKTYPE(lock);
+  PLATFORMLOCKTYPEPTR(hierarchical_lock);
   PLATFORMCONDITIONTYPEPTR(idle);
   size_t position;
   timer_map_t* timers;
@@ -115,7 +116,7 @@ void hierarchical_timing_wheel_stop(hierarchical_timing_wheel_t* wheel);
 void hierarchical_timing_wheel_run(hierarchical_timing_wheel_t* wheel);
 void hierarchical_timing_wheel_simulate(hierarchical_timing_wheel_t* wheel);
 
-timing_wheel_t* timing_wheel_create(uint64_t interval, size_t slot_count, work_pool_t* pool, timer_map_t* timers, PLATFORMCONDITIONTYPEPTR(idle));
+timing_wheel_t* timing_wheel_create(uint64_t interval, size_t slot_count, work_pool_t* pool, timer_map_t* timers, PLATFORMLOCKTYPEPTR(hierarchical_lock), PLATFORMCONDITIONTYPEPTR(idle));
 void timing_wheel_destroy(timing_wheel_t* wheel);
 void timing_wheel_set_timer(timing_wheel_t* wheel, timer_st* timer);
 void timing_wheel_stop(timing_wheel_t* wheel);
diff --git a/src/Util/threadding.h b/src/Util/threadding.h
index 5149188..c67475d 100644
--- a/src/Util/threadding.h
+++ b/src/Util/threadding.h
@@ -13,6 +13,7 @@ extern "C" {
 #if _WIN32
 #include <windows>
 #define PlATFORMLOCKTYPE(N) CRITICAL_SECTION N
+#define PLATFORMLOCKTYPEPTR(N) CRITICAL_SECTION* N
 #define PLATFORMCONDITIONTYPE(N) CONDITION_VARIABLE N
 #define PLATFORMCONDITIONTYPEPTR(N) CONDITION_VARIABLE* N
 #define PLATFORMBARRIERTYPE(N) SYNCHRONIZATION_BARRIER N
@@ -42,6 +43,7 @@ uint64_t platform_self();
 #else
 #include <pthread.h>
 #define PLATFORMLOCKTYPE(N) pthread_mutex_t N
+#define PLATFORMLOCKTYPEPTR(N) pthread_mutex_t* N
 #define PLATFORMCONDITIONTYPE(N) pthread_cond_t N
 #define PLATFORMCONDITIONTYPEPTR(N) pthread_cond_t* N
 #define PLATFORMBARRIERTYPE(N) pthread_barrier_t N
```

## Verification

Build with AddressSanitizer and run under concurrent load:

```bash
cmake -B build_asan -DBUILD_BENCHMARKS=ON -DBUILD_TESTS=ON \
  -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
  -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
cmake --build build_asan -j$(nproc)

# Unit tests
ASAN_OPTIONS=detect_leaks=1 ./build_asan/test_database
ASAN_OPTIONS=detect_leaks=1 ./build_asan/test_mvcc

# Concurrent benchmark (exercises the race)
ASAN_OPTIONS=detect_leaks=0 ./build_asan/benchmark_database --threads 4 --duration 5
```

Before the fix, the benchmark triggers `heap-use-after-free` in `compare_uint64` called from `hashmap_base_put`. After the fix, zero ASAN errors.