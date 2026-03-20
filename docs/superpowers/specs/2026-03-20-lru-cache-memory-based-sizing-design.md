# LRU Cache Memory-Based Sizing Design

**Date:** 2026-03-20
**Author:** Claude (brainstorming session)
**Status:** Draft - Pending User Review

---

## Overview

Replace the current entry-count-based LRU cache with memory-based sizing to improve read-heavy workload performance. The current cache limits to 1,000 entries regardless of entry size, which underutilizes memory for small entries and wastes memory on large entries.

**Goals:**
- Increase cache capacity from 1,000 entries to ~50,000-100,000 entries (50-100x increase)
- Improve read throughput by 2-5x (from reduced trie traversals)
- Achieve 98-99%+ cache hit rate for read-heavy workloads (currently ~90-95%)
- Make cache size configurable via memory budget rather than entry count

**Non-Goals:**
- Dynamic cache sizing (auto-tuning based on hit rate)
- Lock-free cache implementation (future optimization)
- Compression of cached entries

---

## Current State

### Problem
- Fixed entry limit (1,000 entries) regardless of memory
- Small entries waste potential cache space
- Large entries consume disproportionate cache space
- No visibility into actual memory usage
- Default size too small for modern workloads

### Current Implementation
```c
// Current LRU node
struct database_lru_node_t {
    path_t* path;
    identifier_t* value;
    database_lru_node_t* next;
    database_lru_node_t* previous;
};

// Current LRU cache
typedef struct {
    PLATFORMLOCKTYPE(lock);
    HASHMAP(path_t, database_lru_node_t) cache;
    database_lru_node_t* first;
    database_lru_node_t* last;
    size_t size;        // Entry count
    size_t max_size;    // Max entries
} database_lru_cache_t;
```

**Eviction Trigger:** When `size >= max_size`

**Memory Usage:** Unknown (not tracked)

---

## Proposed Design

### Core Data Structure Changes

#### 1. LRU Node with Memory Tracking
```c
typedef struct database_lru_node_t database_lru_node_t;
struct database_lru_node_t {
    path_t* path;           // Key (owned by node)
    identifier_t* value;    // Value (reference counted)
    size_t memory_size;     // NEW: Approximate memory for this entry
    database_lru_node_t* next;
    database_lru_node_t_t* previous;
};
```

**Why:** Track memory per entry to enable memory-based eviction

**Memory Size Includes:**
- Node overhead: `sizeof(database_lru_node_t)`
- Path memory: `path_t` + identifiers + chunks + data
- Value memory: `identifier_t` + chunks + data

#### 2. LRU Cache with Memory Budget
```c
typedef struct {
    PLATFORMLOCKTYPE(lock);
    HASHMAP(path_t, database_lru_node_t) cache;
    database_lru_node_t* first;      // Most recently used
    database_lru_node_t* last;       // Least recently used
    size_t current_memory;            // NEW: Current memory usage in bytes
    size_t max_memory;                // NEW: Maximum memory budget in bytes
    size_t entry_count;               // NEW: Current number of entries
} database_lru_cache_t;
```

**Why:** Replace entry count limit with memory budget

**Key Changes:**
- `size` → `entry_count` (clearer name)
- `max_size` → `max_memory` (bytes instead of count)
- Add `current_memory` to track total usage

---

## Implementation Details

### Memory Calculation

```c
static size_t calculate_entry_memory(path_t* path, identifier_t* value) {
    size_t total = sizeof(database_lru_node_t);

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
                total += chunk->data->size;
            }
        }
    }

    // Value memory (similar calculation)
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

**Approach:** Recursively calculate memory for all nested structures

**Accuracy:** Approximate (excludes allocator overhead, fragmentation)

**Performance:** O(depth) where depth = path length + value chunks

### Eviction Logic

**Current (entry-based):**
```c
if (lru->size >= lru->max_size) {
    evict_lru_entry();
}
```

**Proposed (memory-based):**
```c
size_t entry_memory = calculate_entry_memory(path, value);

// Evict until we have enough space
while (lru->current_memory + entry_memory > lru->max_memory &&
       lru->last != NULL) {
    identifier_t* evicted = lru_evict(lru);
    if (evicted != NULL) {
        identifier_destroy(evicted);
    }
}

// Add entry
node->memory_size = entry_memory;
lru->current_memory += entry_memory;
lru->entry_count++;
```

**Key Difference:** Evict until memory budget allows new entry, not just one entry

**Edge Case:** Single entry larger than `max_memory`
- Allow insertion even if `current_memory + entry_memory > max_memory`
- Next insertion will evict the oversized entry
- Cache temporarily exceeds budget

### Memory Tracking on Operations

**Insert (put):**
1. Calculate memory for new entry
2. Evict LRU entries until budget allows
3. Create node with `memory_size` field
4. Update `current_memory` and `entry_count`

**Delete:**
1. Get node from hashmap
2. Subtract `node->memory_size` from `current_memory`
3. Decrement `entry_count`
4. Remove from hashmap and list
5. Free node

**Evict:**
1. Get LRU node (`lru->last`)
2. Subtract `node->memory_size` from `current_memory`
3. Decrement `entry_count`
4. Remove from hashmap and list
5. Return evicted value (caller destroys)

**Get:**
- No memory changes (reference counting only)
- Update LRU position

---

## API Changes

### Database Create Function

**Before:**
```c
database_t* database_create(
    const char* location,
    size_t lru_size,              // Number of entries
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

**After:**
```c
database_t* database_create(
    const char* location,
    size_t lru_memory_mb,        // Memory budget in MB
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

**Breaking Change:** Yes (parameter semantics changed)

**Migration:** Update all callers to pass MB instead of entry count

### Default Value
```c
#define DATABASE_DEFAULT_LRU_MEMORY_MB 50  // 50 MB default
```

**Rationale:** 50 MB is middle of 10-100 MB range, sufficient for ~50,000-100,000 entries

### LRU Cache Create

**Before:**
```c
database_lru_cache_t* database_lru_cache_create(size_t max_size);
```

**After:**
```c
database_lru_cache_t* database_lru_cache_create(size_t max_memory_bytes);
```

**In database.c:**
```c
// Convert MB to bytes
db->lru_memory_bytes = (lru_memory_mb == 0) ?
    DATABASE_DEFAULT_LRU_MEMORY_MB * 1024 * 1024 :
    lru_memory_mb * 1024 * 1024;

// Create LRU cache
db->lru = database_lru_cache_create(db->lru_memory_bytes);
```

---

## Testing Strategy

### Unit Tests

#### Test 1: Memory Calculation Accuracy
```c
TEST(LRUCache, MemoryCalculation) {
    path_t* path = create_test_path("a", "b");
    identifier_t* value = create_test_identifier("value");

    size_t calculated = calculate_entry_memory(path, value);
    size_t actual = sizeof(database_lru_node_t) +
                    path_memory(path) +
                    identifier_memory(value);

    EXPECT_EQ(calculated, actual);

    path_destroy(path);
    identifier_destroy(value);
}
```

#### Test 2: Memory-Based Eviction
```c
TEST(LRUCache, MemoryBasedEviction) {
    database_lru_cache_t* lru = database_lru_cache_create(1024);  // 1 KB

    // Add entries totaling > 1 KB
    path_t* path1 = create_test_path("key1");
    identifier_t* value1 = create_test_identifier("value1");  // ~100 bytes
    database_lru_cache_put(lru, path1, value1);

    path_t* path2 = create_test_path("key2");
    identifier_t* value2 = create_large_identifier(900);  // 900 bytes
    database_lru_cache_put(lru, path2, value2);

    // Add large entry (should evict path1)
    path_t* path3 = create_test_path("key3");
    identifier_t* value3 = create_large_identifier(900);
    identifier_t* evicted = database_lru_cache_put(lru, path3, value3);

    // Verify eviction
    EXPECT_NE(evicted, nullptr);
    EXPECT_EQ(database_lru_cache_contains(lru, path1), 0);
    EXPECT_EQ(database_lru_cache_contains(lru, path2), 1);

    database_lru_cache_destroy(lru);
}
```

#### Test 3: Memory Tracking Accuracy
```c
TEST(LRUCache, MemoryTracking) {
    database_lru_cache_t* lru = database_lru_cache_create(1024 * 1024);

    // Add entry
    path_t* path1 = create_test_path("key1");
    identifier_t* value1 = create_test_identifier("value1");
    database_lru_cache_put(lru, path1, value1);

    size_t after_put = lru->current_memory;
    EXPECT_GT(after_put, 0);

    // Get entry (no memory change)
    identifier_t* cached = database_lru_cache_get(lru, path1);
    EXPECT_EQ(lru->current_memory, after_put);

    // Delete entry
    database_lru_cache_delete(lru, path1);
    EXPECT_EQ(lru->current_memory, 0);

    database_lru_cache_destroy(lru);
}
```

#### Test 4: Variable Size Entries
```c
TEST(LRUCache, VariableSizeEntries) {
    database_lru_cache_t* lru = database_lru_cache_create(10 * 1024);  // 10 KB

    // Many small entries
    for (int i = 0; i < 100; i++) {
        path_t* path = create_test_path("key%d", i);
        identifier_t* value = create_test_identifier("v");
        database_lru_cache_put(lru, path, value);
    }

    size_t small_count = lru->entry_count;
    EXPECT_GT(small_count, 50);

    database_lru_cache_destroy(lru);

    // Few large entries
    lru = database_lru_cache_create(10 * 1024);
    for (int i = 0; i < 100; i++) {
        path_t* path = create_test_path("key%d", i);
        identifier_t* value = create_large_identifier(1000);
        database_lru_cache_put(lru, path, value);
    }

    size_t large_count = lru->entry_count;
    EXPECT_LT(large_count, 15);

    database_lru_cache_destroy(lru);
}
```

### Edge Cases

#### Test 5: Zero Memory Budget
```c
TEST(LRUCache, ZeroMemoryBudget) {
    database_lru_cache_t* lru = database_lru_cache_create(0);
    EXPECT_EQ(lru->max_memory, DATABASE_DEFAULT_LRU_MEMORY_MB * 1024 * 1024);
    database_lru_cache_destroy(lru);
}
```

#### Test 6: Entry Larger Than Max Memory
```c
TEST(LRUCache, EntryLargerThanMaxMemory) {
    database_lru_cache_t* lru = database_lru_cache_create(100);  // 100 bytes

    // Add 1 KB entry
    path_t* path = create_test_path("large");
    identifier_t* value = create_large_identifier(1024);
    database_lru_cache_put(lru, path, value);

    // Entry should be cached (temporary over budget)
    EXPECT_EQ(lru->entry_count, 1);
    EXPECT_GT(lru->current_memory, lru->max_memory);

    // Next insert should evict
    path_t* path2 = create_test_path("small");
    identifier_t* value2 = create_test_identifier("x");
    database_lru_cache_put(lru, path2, value2);

    EXPECT_EQ(database_lru_cache_contains(lru, path), 0);

    database_lru_cache_destroy(lru);
}
```

### Integration Tests

#### Test 7: Performance Benchmark
```cpp
TEST_F(DatabaseBenchmark, LRUCacheMemoryBased) {
    database_t* db = database_create(
        "/tmp/test_db",
        50,  // 50 MB cache
        0, 0, 0, 0, 0,
        pool, wheel, &error
    );

    // Pre-populate 100K entries
    for (int i = 0; i < 100000; i++) {
        // Insert entries
    }

    // Read-heavy workload (70/20/10)
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 10000; i++) {
        if (i % 10 < 7) database_get(...);
        else if (i % 10 < 9) database_put(...);
        else database_delete(...);
    }
    auto end = std::chrono::high_resolution_clock::now();

    // Measure: ops/sec, cache hit rate, memory usage

    database_destroy(db);
}
```

---

## Files to Modify

### 1. src/Database/database_lru.h
- Add `memory_size` field to `database_lru_node_t`
- Add `current_memory`, `max_memory`, `entry_count` to `database_lru_cache_t`
- Update `database_lru_cache_create()` signature

### 2. src/Database/database_lru.c
- Implement `calculate_entry_memory()`
- Update `database_lru_cache_create()` to use `max_memory_bytes`
- Update `database_lru_cache_put()` with memory-based eviction
- Update `database_lru_cache_delete()` to track memory
- Update `lru_evict()` to update memory tracking
- Update `database_lru_cache_destroy()` to use `entry_count`

### 3. src/Database/database.h
- Add `DATABASE_DEFAULT_LRU_MEMORY_MB` constant
- Update `database_create()` signature (change `lru_size` to `lru_memory_mb`)

### 4. src/Database/database.c
- Update `database_create()` to convert MB to bytes
- Update LRU cache creation call

### 5. tests/test_database_lru.cpp (NEW)
- Create comprehensive unit tests for memory-based eviction
- Test memory calculation accuracy
- Test edge cases

### 6. tests/test_database.cpp
- Update to use `lru_memory_mb` parameter
- Add cache hit rate metrics

### 7. benchmarks/benchmark_database.cpp
- Update to use `lru_memory_mb` parameter
- Benchmark different cache sizes

---

## Performance Expectations

### Before (1,000 entries)
```
Cache hit rate: 90-95%
Read ops/sec:   26,626
Cache evictions: Frequent (small cache)
Memory usage:   Unknown
```

### After (50 MB = ~50,000-100,000 entries)
```
Cache hit rate: 98-99%+ (expected)
Read ops/sec:   53,000-133,000 (2-5x improvement)
Cache evictions: Rare (large cache)
Memory usage:   ~50 MB (predictable)
```

### Factors Affecting Performance
1. **Entry size distribution:** Smaller entries = more cache hits
2. **Access pattern:** Read-heavy = better cache utilization
3. **Working set size:** Active keys < cache capacity = high hit rate
4. **Memory budget:** 50 MB should cover most workloads

### Benchmarks to Run
1. **Before/after comparison:** Same workload, different cache sizing
2. **Cache size sweep:** 10 MB, 25 MB, 50 MB, 100 MB
3. **Hit rate analysis:** Measure cache hit/miss ratio
4. **Memory profiling:** Verify memory tracking accuracy

---

## Implementation Order

### Phase 1: Data Structure Changes
- [ ] Update `database_lru_node_t` to add `memory_size`
- [ ] Update `database_lru_cache_t` to track memory and entry count

### Phase 2: Helper Functions
- [ ] Implement `calculate_entry_memory()`
- [ ] Add memory tracking utilities

### Phase 3: Core Logic
- [ ] Update `database_lru_cache_create()` to accept `max_memory_bytes`
- [ ] Update `database_lru_cache_put()` with memory-based eviction
- [ ] Update `database_lru_cache_delete()` to track memory
- [ ] Update `lru_evict()` to update memory tracking

### Phase 4: Database Integration
- [ ] Update `database_create()` API
- [ ] Update all test files

### Phase 5: Testing
- [ ] Write new unit tests
- [ ] Run existing tests (no regressions)
- [ ] Run performance benchmarks

### Phase 6: Validation
- [ ] Verify memory tracking accuracy
- [ ] Measure cache hit rate improvement
- [ ] Profile memory usage

---

## Risks and Mitigations

### Risk 1: Memory Calculation Inaccuracy
**Impact:** Cache uses more/less memory than budget
**Mitigation:**
- Use conservative estimates (over-estimate slightly)
- Document that calculation is approximate
- Add validation tests comparing calculated vs actual memory

### Risk 2: Breaking API Changes
**Impact:** All callers must update parameter
**Mitigation:**
- This is an internal project, not public API
- Update all callers in tests and examples
- Document breaking change in CHANGELOG

### Risk 3: Oversized Entries
**Impact:** Single entry larger than `max_memory` causes temporary over-budget
**Mitigation:**
- Allow temporary over-budget state
- Next insertion will evict oversized entry
- Document behavior in header comments

### Risk 4: Performance Regression
**Impact:** Memory calculation overhead slows down operations
**Mitigation:**
- Memory calculation is O(depth) where depth is small
- Cache hit improvement outweighs calculation overhead
- Profile to ensure overhead < 5% of operation time

---

## Success Criteria

- [ ] All unit tests pass
- [ ] No regressions in existing tests
- [ ] Memory tracking accurate within 10%
- [ ] Cache hit rate improves from ~90-95% to ~98-99%+
- [ ] Read throughput improves by 2-5x
- [ ] Memory usage predictable and within budget

---

## Future Optimizations

Not in scope for this design, but potential improvements:

1. **Adaptive cache sizing:** Auto-tune based on hit rate
2. **Lock-free LRU:** Use RCU for concurrent reads
3. **Cache warming:** Pre-populate on database load
4. **Compression:** Compress cached values to save memory
5. **Tiered cache:** Hot/warm/cold tiers with different sizes

---

## Questions for Reviewer

1. **Is 50 MB the right default?** Should it be higher/lower?
2. **Should we expose cache statistics?** (hit rate, memory usage)
3. **Do we need a max entry count limit too?** (hybrid approach)
4. **Should we validate memory budget is reasonable?** (e.g., reject < 1 MB)