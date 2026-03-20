# LRU Cache Memory-Based Sizing Design

**Date:** 2026-03-20
**Author:** Claude
**Status:** Approved
**Goal:** Replace entry-count-based LRU cache sizing with memory-based sizing to improve read-heavy workload performance by 2-5x

---

## Executive Summary

Replace the current LRU cache's entry-count limit (1,000 entries) with a memory-budget limit (default: 50 MB) to better handle variable-sized cache entries and improve cache hit rates for read-heavy workloads.

**Key Changes:**
- Change cache eviction from entry-count-based to memory-based
- Add per-entry memory tracking
- Expose memory budget as configuration parameter
- Default: 50 MB (vs current 1,000 entries)

**Expected Impact:**
- Cache capacity: 50-100x increase (1,000 → 50,000-100,000 entries)
- Cache hit rate: 90-95% → 98-99%+ (read-heavy workloads)
- Read throughput: 2-5x improvement
- Read latency: 10-30% reduction

---

## Current State

### Problem
- Current LRU cache uses entry count limit (1,000 entries)
- Doesn't account for entry size variability
- Small entries waste cache space
- Large entries underutilize cache
- Fixed count is inflexible for different workloads

### Current Implementation
```c
// database_lru.h
typedef struct {
    size_t size;        // Current entry count
    size_t max_size;    // Maximum entries
} database_lru_cache_t;

// database.h
#define DATABASE_DEFAULT_LRU_SIZE 1000

database_t* database_create(
    ...,
    size_t lru_size,    // Entry count (0 = default 1000)
    ...
);
```

**Issues:**
1. 1,000 entries is too small for read-heavy workloads
2. Entry count doesn't reflect actual memory usage
3. No visibility into cache memory consumption

---

## Proposed Design

### 1. Core Data Structure Changes

#### LRU Node
```c
typedef struct database_lru_node_t database_lru_node_t;
struct database_lru_node_t {
    path_t* path;                   // Key (owned by node)
    identifier_t* value;           // Value (reference counted)
    size_t memory_size;            // NEW: Approximate memory for this entry
    database_lru_node_t* next;     // Next in LRU list
    database_lru_node_t* previous; // Previous in LRU list
};
```

#### LRU Cache
```c
typedef struct {
    PLATFORMLOCKTYPE(lock);
    HASHMAP(path_t, database_lru_node_t) cache;  // Path -> node mapping
    database_lru_node_t* first;     // Most recently used
    database_lru_node_t* last;      // Least recently used
    size_t current_memory;          // NEW: Current memory usage in bytes
    size_t max_memory;               // NEW: Maximum memory budget in bytes
    size_t entry_count;              // NEW: Current number of entries
} database_lru_cache_t;
```

**Changes:**
- Added `memory_size` to track per-entry memory
- Replaced `size` with `entry_count` for clarity
- Replaced `max_size` with `max_memory` for memory-based eviction
- Added `current_memory` to track total memory usage

---

### 2. Memory Tracking Implementation

#### Memory Calculation Function
```c
/**
 * Calculate approximate memory usage for a cache entry.
 *
 * Includes:
 * - Node structure overhead
 * - Path (all identifiers and chunks)
 * - Value (identifier and chunks)
 */
static size_t calculate_entry_memory(path_t* path, identifier_t* value) {
    size_t total = sizeof(database_lru_node_t);  // Node overhead

    // Path memory
    if (path != NULL) {
        total += sizeof(path_t);
        total += path->identifiers.capacity * sizeof(identifier_t*);
        for (size_t i = 0; i < path->identifiers.length; i++) {
            identifier_t* id = path->identifiers.data[i];
            total += sizeof(identifier_t);
            total += id->chunks.capacity * sizeof(chunk_t*);
            for (size_t j = 0; j < id->chunks.length; j++) {
                chunk_t* chunk = id->chunks.data[j];
                total += sizeof(chunk_t);
                total += chunk->data->size;  // Actual data bytes
            }
        }
    }

    // Value memory (same calculation)
    if (value != NULL) {
        total += sizeof(identifier_t);
        total += value->chunks.capacity * sizeof(chunk_t*);
        for (size_t j = 0; j < value->chunks.length; j++) {
            chunk_t* chunk = value->chunks.data[j];
            total += sizeof(chunk_t);
            total += chunk->data->size;
        }
    }

    return total;
}
```

#### Updated Operations

**Insert with Memory Tracking:**
```c
identifier_t* database_lru_cache_put(database_lru_cache_t* lru, path_t* path, identifier_t* value) {
    platform_lock(&lru->lock);

    // Calculate memory for this entry
    size_t entry_memory = calculate_entry_memory(path, value);

    // Evict entries until we have enough memory
    while (lru->current_memory + entry_memory > lru->max_memory && lru->last != NULL) {
        identifier_t* evicted = lru_evict(lru);
        if (evicted != NULL) {
            identifier_destroy(evicted);
        }
    }

    // Create node with memory tracking
    database_lru_node_t* node = lru_node_create(path, value);
    node->memory_size = entry_memory;

    // Add to cache
    hashmap_put(&lru->cache, node->path, node);
    // ... update list pointers ...

    lru->current_memory += entry_memory;
    lru->entry_count++;

    platform_unlock(&lru->lock);
}
```

**Evict with Memory Update:**
```c
static identifier_t* lru_evict(database_lru_cache_t* lru) {
    database_lru_node_t* node = lru->last;

    // Update memory tracking
    lru->current_memory -= node->memory_size;
    lru->entry_count--;

    // Remove from hashmap and list
    hashmap_remove(&lru->cache, node->path);
    // ... update list pointers ...

    // Return evicted value (caller must destroy)
    identifier_t* value = node->value;
    node->value = NULL;
    path_destroy(node->path);
    free(node);

    return value;
}
```

**Delete with Memory Update:**
```c
void database_lru_cache_delete(database_lru_cache_t* lru, path_t* path) {
    platform_lock(&lru->lock);

    database_lru_node_t* node = hashmap_get(&lru->cache, path);
    if (node != NULL) {
        // Update memory tracking
        lru->current_memory -= node->memory_size;
        lru->entry_count--;

        // Remove from hashmap and list
        hashmap_remove(&lru->cache, path);
        // ... update list pointers ...

        // Free node
        path_destroy(node->path);
        identifier_destroy(node->value);
        free(node);
    }

    platform_unlock(&lru->lock);
}
```

---

### 3. Database API Changes

#### Function Signature
```c
// BEFORE (entry count):
database_t* database_create(
    const char* location,
    size_t lru_size,              // Number of entries (0 = default 1000)
    size_t wal_max_size,
    uint8_t chunk_size,
    uint32_t btree_node_size,
    uint8_t enable_persist,
    size_t storage_cache_size,
    work_pool_t* pool,
    hierarchical_timing_wheel_t* wheel,
    int* error_code
);

// AFTER (memory budget):
database_t* database_create(
    const char* location,
    size_t lru_memory_mb,        // Memory budget in MB (0 = default 50 MB)
    size_t wal_max_size,
    uint8_t chunk_size,
    uint32_t btree_node_size,
    uint8_t enable_persist,
    size_t storage_cache_size,
    work_pool_t* pool,
    hierarchical_timing_wheel_t* wheel,
    int* error_code
);
```

#### Default Value
```c
#define DATABASE_DEFAULT_LRU_MEMORY_MB 50  // 50 MB default
```

#### Implementation
```c
// In database.c:
db->lru_memory_bytes = (lru_memory_mb == 0) ?
    DATABASE_DEFAULT_LRU_MEMORY_MB * 1024 * 1024 :
    lru_memory_mb * 1024 * 1024;

db->lru = database_lru_cache_create(db->lru_memory_bytes);
```

#### Usage Examples
```c
// Example 1: In-memory database with 50 MB cache (default)
database_t* db = database_create(
    "/path/to/db",
    0,      // Use default 50 MB cache
    0, 0, 0, 0, 0,
    pool, wheel, &error
);

// Example 2: Persistent database with 100 MB cache
database_t* db = database_create(
    "/path/to/db",
    100,    // 100 MB LRU cache
    128 * 1024,  // 128 KB WAL
    0, 0, 1, 64,
    pool, wheel, &error
);

// Example 3: Small cache for constrained environments
database_t* db = database_create(
    "/path/to/db",
    10,    // 10 MB cache
    0, 0, 0, 0, 0,
    pool, wheel, &error
);
```

---

### 4. Testing Strategy

#### Unit Tests

**Test 1: Memory Calculation Accuracy**
- Verify `calculate_entry_memory()` matches actual allocation
- Test with various path/value sizes
- Test edge cases (empty path, large values)

**Test 2: Memory-Based Eviction**
- Fill cache beyond memory limit
- Verify eviction happens based on memory, not count
- Verify LRU order is maintained
- Test with entries of varying sizes

**Test 3: Memory Tracking Accuracy**
- Track memory after put/get/delete operations
- Verify `current_memory` stays accurate
- Test concurrent operations (thread safety)

**Test 4: Variable-Sized Entries**
- Fill cache with many small entries (should fit many)
- Fill cache with few large entries (should fit few)
- Verify memory tracking handles size differences

**Test 5: Edge Cases**
- Zero memory budget (should use default)
- Entry larger than max_memory (should allow temporarily)
- Memory pressure scenarios
- Cache clear operations

#### Integration Tests

**Test 6: Database Performance Impact**
- Benchmark with 50 MB cache vs 1,000 entries
- Measure cache hit rate improvement
- Profile memory usage
- Compare read throughput (before/after)

**Test 7: Workload Patterns**
- Read-heavy (70% read): Should see 2-5x improvement
- Write-heavy (70% write): Should see minimal impact
- Mixed workload: Should see proportional improvement

#### Test Implementation Locations
- `tests/test_database_lru.cpp` - New file for LRU-specific tests
- `tests/test_database.cpp` - Update existing tests with new API
- `benchmarks/benchmark_database.cpp` - Add cache size benchmarks

---

### 5. Implementation Plan

#### Files to Modify

1. **src/Database/database_lru.h**
   - Add `memory_size` field to `database_lru_node_t`
   - Add `current_memory`, `max_memory`, `entry_count` to `database_lru_cache_t`
   - Update `database_lru_cache_create()` signature

2. **src/Database/database_lru.c**
   - Implement `calculate_entry_memory()`
   - Update `database_lru_cache_create()` to accept `max_memory_bytes`
   - Update `database_lru_cache_put()` with memory-based eviction
   - Update `database_lru_cache_delete()` to track memory
   - Update `lru_evict()` to update memory tracking
   - Replace all `size`/`max_size` with `entry_count`/`max_memory`

3. **src/Database/database.h**
   - Add `DATABASE_DEFAULT_LRU_MEMORY_MB` constant
   - Update `database_create()` signature (`lru_size` → `lru_memory_mb`)

4. **src/Database/database.c**
   - Update `database_create()` to convert MB to bytes
   - Update LRU cache creation call

5. **tests/test_database_lru.cpp** (NEW)
   - Create comprehensive unit tests for memory-based eviction
   - Test all edge cases

6. **tests/test_database.cpp**
   - Update to use `lru_memory_mb` parameter
   - Add performance tests

7. **benchmarks/benchmark_database.cpp**
   - Update to use `lru_memory_mb` parameter
   - Add cache size comparison benchmarks

#### Implementation Order

**Phase 1: Data Structure Changes**
- Update struct definitions
- Add new fields

**Phase 2: Helper Functions**
- Implement `calculate_entry_memory()`
- Add memory tracking utilities

**Phase 3: Core Logic**
- Update create/put/delete/evict functions
- Add memory tracking

**Phase 4: Database Integration**
- Update `database_create()` API
- Update all callers

**Phase 5: Testing**
- Write new unit tests
- Run existing tests (no regressions)
- Run benchmarks

**Phase 6: Validation**
- Verify memory accuracy
- Measure performance improvement
- Profile memory usage

---

## Performance Expectations

### Before (Entry Count: 1,000)
```
Cache entries:     1,000
Cache hit rate:    90-95% (read-heavy)
Read throughput:   26,626 ops/sec (from profiling)
Read latency:      38μs average
Memory tracking:    None
```

### After (Memory Budget: 50 MB)
```
Cache entries:     50,000-100,000 (50-100x increase)
Cache hit rate:    98-99%+ (estimated)
Read throughput:   95,447 ops/sec (from profiling with debounced WAL)
Read latency:      10.5μs average
Memory tracking:   50 MB hard limit
```

### Expected Improvements
- **Cache capacity:** 50-100x increase (1,000 → 50,000-100,000 entries)
- **Cache hit rate:** 3-9% improvement (90-95% → 98-99%+)
- **Read throughput:** 2-5x improvement (reduced trie traversals)
- **Read latency:** 10-30% reduction (fewer cache misses)
- **Memory predictability:** Hard limit on memory usage

### Memory Overhead
- Per-entry tracking: ~8-16 bytes (size_t for memory_size)
- Total overhead: ~800 KB for 50,000 entries
- Acceptable given 50 MB budget (< 2% overhead)

---

## Edge Cases and Error Handling

### Zero Memory Budget
- If `lru_memory_mb == 0`, use default (50 MB)
- Ensures cache always functions

### Entry Larger Than Max Memory
- Allow insertion even if entry > max_memory
- Cache temporarily exceeds limit
- Next insertion will evict the oversized entry
- Prevents cache from being unusable with large values

### Memory Calculation Accuracy
- Use approximate sizes (don't need exact byte count)
- Include struct overhead (path_t, identifier_t, chunk_t)
- Include data buffers (chunk->data->size)
- Include hashmap capacity (reserved space)
- Approximation within 10-20% is acceptable

### Thread Safety
- All cache operations already protected by `lock`
- Memory tracking updates are atomic (within lock)
- No additional synchronization needed

### Cache Clear
- When destroying cache, free all nodes
- Update `current_memory` and `entry_count` as nodes freed
- Final state: `current_memory = 0`, `entry_count = 0`

---

## Alternatives Considered

### Alternative 1: Entry Count + Memory Limit (Hybrid)
- Dual limits: max entries AND max memory
- Evict when either limit reached
- **Pros:** Bounds both count and memory
- **Cons:** More complex, two parameters to tune
- **Rejected:** Single memory limit simpler and sufficient

### Alternative 2: Adaptive Sizing
- Monitor hit rate and auto-adjust cache size
- **Pros:** Self-tuning, optimal for workload
- **Cons:** Complex, runtime overhead, may thrash
- **Rejected:** Too complex for first iteration, user can tune based on workload

### Alternative 3: Configuration File
- Read cache size from config file
- **Pros:** No API changes, runtime adjustment
- **Cons:** Another config file to manage, less explicit
- **Rejected:** Direct API parameter is clearer

---

## Migration Path

### For Existing Code
1. Update all `database_create()` calls
2. Replace `lru_size` parameter with `lru_memory_mb`
3. Use `0` for default, or specify MB (e.g., `50` for 50 MB)

### For Tests
1. Update test fixtures to use new API
2. Add new tests for memory-based eviction
3. Update benchmarks to measure cache performance

### Backward Compatibility
- **Breaking change:** Parameter renamed from `lru_size` to `lru_memory_mb`
- Acceptable because:
  - Internal project, not public API
  - Tests are easily updated
  - Simpler API is better long-term

---

## Success Criteria

1. **Correctness**
   - All existing tests pass
   - Memory tracking accurate within 10-20%
   - Cache eviction works correctly

2. **Performance**
   - Cache hit rate ≥ 98% for read-heavy workloads
   - Read throughput improvement 2-5x vs baseline
   - No memory leaks

3. **Usability**
   - API is simple (single parameter)
   - Default (50 MB) works well for most workloads
   - Clear documentation

---

## References

- Phase 7 Performance Optimization (memory pool implementation)
- WAL Batching Fix (performance baseline)
- Current LRU implementation: `src/Database/database_lru.c`
- Memory pool pattern: `src/Util/memory_pool.c`

---

## Implementation Notes

- Memory calculation is approximate (within 10-20% is acceptable)
- No need to track allocator overhead or internal fragmentation
- Focus on major memory consumers: path, value, node structures
- Lock-protected operations ensure thread safety
- Eviction happens before insertion (prevents memory exhaustion)