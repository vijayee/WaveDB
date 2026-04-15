# Timing Wheel Reference Counting: Design and Implementation

## Problem

The timing wheel stored timer pointers in two data structures simultaneously —
the `wheel->timers` hashmap and sub-wheel slot lists — but had no ownership
tracking. When `hierarchical_timing_wheel_stop` freed timers from the hashmap,
the slot lists still held dangling pointers, causing double-free crashes. A
naive fix of "mark removed but don't free" avoided the crash but introduced a
memory leak: cancelled timers stayed in slot lists until the wheel position
wrapped around, and during shutdown the cleanup paths (`timing_wheel_worker_abort`
and `timer_list_destroy`) both tried to free the same timer.

## Solution: Reference Counting with `refcounter_t`

Each `timer_st` now has a `refcounter_t refcounter` as its first member,
following the project's existing pattern (see CLAUDE.md: "Reference-counted
structs have `refcounter_t refcounter` as the first member").

### Ownership Model

A timer has two owners, each contributing one reference:

| Owner | When reference is added | When reference is released |
|-------|------------------------|---------------------------|
| Hashmap | `timer_ref` after `hashmap_put` in `hierarchical_timing_wheel_set_timer` | `timer_unref` after `hashmap_remove` in cancel/maintenance/stop |
| Slot list | `timer_ref` inside `timing_wheel_set_timer` (after `timer_list_enqueue`) | `timer_unref` when removed from slot list (maintenance/abort/destroy) |

When both references are released, `refcounter_count` reaches 0 and the timer
is freed via `timer_destroy` (which frees `plan.steps`, destroys the
refcounter lock, and frees the timer struct).

A third **local reference** is used for the expired-list transfer in
`timing_wheel_maintenance`:
- `timer_ref` before moving to expired list (prevents premature free)
- `timer_unref` in `timing_wheel_fire_expired` (after firing or re-enqueueing)

### Lifecycle Diagrams

**User timer — normal fire (no more plan steps):**
```
hierarchical_timing_wheel_set_timer:
  refcounter_init → count=1 (creator)
  hashmap_put → timer_ref → count=2
  timing_wheel_set_timer → timer_ref → count=3  (inside: timer_list_enqueue + ref)
  timer_unref → count=2 (creator released)

timing_wheel_maintenance:
  timer_ref → count=3 (local ref for expired transfer)
  hashmap_remove → timer_unref → count=2
  timer_list_remove → timer_unref → count=1

timing_wheel_fire_expired:
  timer_unref → count=0 → timer_destroy  (local ref released, final fire)
```

**User timer — cancelled:**
```
hierarchical_timing_wheel_set_timer:  count=2 (hashmap + slot list)

hierarchical_timing_wheel_cancel_timer:
  timer->removed = 1
  hashmap_remove → timer_unref → count=1

timing_wheel_maintenance (encounters removed timer):
  timer_list_remove → timer_unref → count=0 → timer_destroy
```

**Internal tick timer (not in hashmap):**
```
timing_wheel_run:
  refcounter_init → count=1 (creator)
  timing_wheel_set_timer → timer_ref → count=2  (inside: timer_list_enqueue + ref)
  timer_unref → count=1 (creator released)

timing_wheel_maintenance:
  timer_ref → count=2 (local ref)
  (no hashmap_remove — not in hashmap)
  timer_list_remove → timer_unref → count=1

timing_wheel_fire_expired:
  timer_unref → count=0 → timer_destroy
```

**Multi-step timer (re-enqueued to next wheel):**
```
timing_wheel_maintenance:  count=1 (local ref, after hashmap+slot removed)

timing_wheel_fire_expired:
  timing_wheel_set_timer → timer_ref → count=2  (new slot list)
  timer_unref → count=1 (local ref released, slot list keeps it alive)
```

### Helper Functions

```c
// Destroy a timer when its refcount reaches 0
static void timer_destroy(timer_st* timer) {
  if (timer == NULL) return;
  free(timer->plan.steps);
  refcounter_destroy_lock((refcounter_t*) timer);
  free(timer);
}

// Decrement refcount; free if count reaches 0
static void timer_unref(timer_st* timer) {
  if (timer == NULL) return;
  refcounter_dereference((refcounter_t*) timer);
  if (refcounter_count((refcounter_t*) timer) == 0) {
    timer_destroy(timer);
  }
}

// Increment refcount; return timer pointer
static timer_st* timer_ref(timer_st* timer) {
  if (timer == NULL) return NULL;
  return (timer_st*) refcounter_reference((refcounter_t*) timer);
}
```

### Key Changes

| File | Change |
|------|--------|
| `wheel.h` | Added `refcounter_t refcounter` as first member of `timer_st` |
| `wheel.c` | Added `timer_destroy`, `timer_unref`, `timer_ref` helpers |
| `wheel.c` | `hierarchical_timing_wheel_set_timer`: `refcounter_init`, `timer_ref` for hashmap, `timer_unref` for creator release |
| `wheel.c` | `timing_wheel_run`: `refcounter_init`, `timer_unref` for creator release (slot ref added inside `timing_wheel_set_timer`) |
| `wheel.c` | `timing_wheel_set_timer`: `timer_ref` after `timer_list_enqueue` |
| `wheel.c` | `hierarchical_timing_wheel_cancel_timer`: `timer_unref` after `hashmap_remove` |
| `wheel.c` | `hierarchical_timing_wheel_stop`: `timer_unref` per timer in hashmap (replaces `free`) |
| `wheel.c` | `timing_wheel_maintenance`: `timer_ref` for local ref, `timer_unref` for hashmap (conditional) and slot list removals, removed timers get `timer_unref` instead of `free` |
| `wheel.c` | `timing_wheel_fire_expired`: `timer_unref` instead of `free`; multi-step path: `timer_unref` after `timing_wheel_set_timer` (which added a slot ref) |
| `wheel.c` | `timing_wheel_worker_abort`: `timer_unref` instead of `free` |
| `wheel.c` | `timer_list_destroy`: `timer_unref` instead of `free(current->timer)` |

### Invariant

A timer's `refcounter_count` equals the number of data structures that hold a
reference to it (hashmap: 0 or 1, slot lists: 0 or 1, local/transfer: 0 or 1).
When count reaches 0, `timer_destroy` frees the timer. This ensures:
- No double-free: each owner releases exactly once
- No leak: all references are eventually released during normal operation or shutdown
- No use-after-free: a timer is never accessed after its count reaches 0

### Verification

```bash
cmake --build . -- -j$(nproc) && ctest --output-on-failure
```

All 21 tests pass. For thorough validation, build with AddressSanitizer:

```bash
cmake -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_FLAGS="-fsanitize=address,undefined" \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" ..
cmake --build . -- -j$(nproc) && ctest --output-on-failure
```