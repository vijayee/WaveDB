# Write Optimization Phase 2: WAL + LRU

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace CBOR WAL encoding with a fixed binary format (big-endian for portability), replace the debouncer with a per-thread write buffer and one-shot timer, and eliminate path deep copies in the LRU cache via reference counting. Estimated cumulative ~2x single-threaded improvement from baseline.

**Architecture:** The binary WAL format replaces libcbor with a compact fixed-format encoding using `write_be16`/`write_be32` helpers. The batch writev accumulates entries in a thread-local buffer and flushes on buffer-full or timeout, eliminating per-write cancel+reschedule timer overhead. The LRU path change uses `path_reference()` instead of `path_copy()` and stores the path hash for O(1) comparison.

**Tech Stack:** C11 (atomic primitives), existing memory pool, big-endian serialization helpers

**Spec:** `docs/superpowers/specs/2026-04-16-write-optimization-design.md` — Phase 2

**Prerequisite:** Phase 1 must be complete (atomic refcounts required for `path_reference()`).

---

## File Structure

| File | Responsibility |
|------|---------------|
| `src/Util/endian.h` | Big-endian read/write helpers (new) |
| `src/Database/wal.h` | WAL format version enum |
| `src/Database/wal.c` | Binary WAL encoding/decoding, format version detection |
| `src/Database/wal_manager.h` | `thread_wal_t` buffer fields, flush function declarations |
| `src/Database/wal_manager.c` | Batch writev logic, one-shot timer, buffer flush |
| `src/Database/database.c` | Replace `encode_put_entry` with binary encoding |
| `src/Database/database_lru.h` | `database_lru_node_t` with `key_hash` and `entry_size` |
| `src/Database/database_lru.c` | Refcounted path storage, hash-first comparison |
| `src/HBTrie/path.h` | `path_reference()` declaration |
| `src/HBTrie/path.c` | `path_reference()` implementation |

---

### Task 1: Binary WAL Format — Endian Helpers

**Files:**
- Create: `src/Util/endian.h`

- [ ] **Step 1: Create `src/Util/endian.h`**

```c
#ifndef WAVEDB_ENDIAN_H
#define WAVEDB_ENDIAN_H

#include <stdint.h>
#include <string.h>

/* Big-endian (network byte order) read/write helpers.
 * All multi-byte integers in WAL entries and bnode serialization
 * are stored big-endian for cross-platform portability.
 * These byte-by-byte implementations avoid platform-specific headers. */

static inline uint16_t write_be16(uint8_t* buf, uint16_t val) {
    buf[0] = (uint8_t)((val >> 8) & 0xFF);
    buf[1] = (uint8_t)(val & 0xFF);
    return val;
}

static inline uint16_t read_be16(const uint8_t* buf) {
    return ((uint16_t)buf[0] << 8) | (uint16_t)buf[1];
}

static inline uint32_t write_be32(uint8_t* buf, uint32_t val) {
    buf[0] = (uint8_t)((val >> 24) & 0xFF);
    buf[1] = (uint8_t)((val >> 16) & 0xFF);
    buf[2] = (uint8_t)((val >> 8) & 0xFF);
    buf[3] = (uint8_t)(val & 0xFF);
    return val;
}

static inline uint32_t read_be32(const uint8_t* buf) {
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
}

#endif /* WAVEDB_ENDIAN_H */
```

- [ ] **Step 2: Commit endian helpers**

```bash
git add src/Util/endian.h
git commit -m "feat: add big-endian read/write helpers for portable binary format

Byte-by-byte implementations avoid platform-specific header issues.
Used by WAL binary encoding and bnode serialization."
```

---

### Task 2: Binary WAL Format — Encoding

**Files:**
- Modify: `src/Database/wal.h`
- Modify: `src/Database/database.c`
- Modify: `src/Database/wal_manager.c`

- [ ] **Step 1: Add WAL format version to wal.h**

Open `src/Database/wal.h`. Add a format version enum:

```c
typedef enum {
    WAL_FORMAT_CBOR = 0,     /* Legacy CBOR-encoded entries */
    WAL_FORMAT_BINARY = 1,   /* Fixed binary format (big-endian) */
} wal_format_e;
```

Add a `format_version` field to the WAL manifest struct or the thread WAL creation path so new WALs are written in binary format.

- [ ] **Step 2: Create `encode_put_entry_binary` in database.c**

Open `src/Database/database.c`. Find `encode_put_entry` (around line 78). Add a new function `encode_put_entry_binary` that encodes the path and value in the binary format described in the spec:

```
[path_count:2B BE][path_len:4B BE]
For each identifier:
  [id_len:2B BE][id_data:id_len bytes]
[value_len:4B BE][value:value_len bytes]
```

```c
static buffer_t* encode_put_entry_binary(path_t* path, identifier_t* value) {
    if (path == NULL || value == NULL) return NULL;

    // Calculate total size
    uint16_t path_count = (uint16_t)path_length(path);
    size_t path_data_size = 0;
    for (size_t i = 0; i < path_count; i++) {
        identifier_t* ident = path_get(path, i);
        buffer_t* ident_buf = identifier_to_buffer(ident);
        path_data_size += 2 + ident_buf->size;  // id_len + data
        buffer_destroy(ident_buf);
    }

    buffer_t* value_buf = identifier_to_buffer(value);
    size_t total_size = 2 + 4 + path_data_size + 4 + value_buf->size;

    uint8_t* data = get_memory(total_size);
    if (data == NULL) { buffer_destroy(value_buf); return NULL; }

    size_t pos = 0;

    // path_count (2 bytes BE)
    write_be16(data + pos, path_count);
    pos += 2;

    // path_len (4 bytes BE) — total path data size for validation
    write_be32(data + pos, (uint32_t)path_data_size);
    pos += 4;

    // Each identifier: id_len (2 bytes BE) + id_data
    for (size_t i = 0; i < path_count; i++) {
        identifier_t* ident = path_get(path, i);
        buffer_t* ident_buf = identifier_to_buffer(ident);
        write_be16(data + pos, (uint16_t)ident_buf->size);
        pos += 2;
        memcpy(data + pos, ident_buf->data, ident_buf->size);
        pos += ident_buf->size;
        buffer_destroy(ident_buf);
    }

    // value_len (4 bytes BE) + value
    write_be32(data + pos, (uint32_t)value_buf->size);
    pos += 4;
    memcpy(data + pos, value_buf->data, value_buf->size);
    pos += value_buf->size;
    buffer_destroy(value_buf);

    buffer_t* result = buffer_create_from_existing_memory(data, pos);
    return result;
}
```

Also add `#include "Util/endian.h"` at the top of the file.

- [ ] **Step 3: Replace `encode_put_entry` calls with `encode_put_entry_binary`**

In `database_put_sync`, `database_delete_sync`, and `database_write_batch_sync`, replace calls to `encode_put_entry` with `encode_put_entry_binary`. Also update `encode_delete_entry` if it exists (use the same binary format with `WAL_DELETE` type).

- [ ] **Step 4: Add binary decoding in wal_manager.c**

In `thread_wal_read` (or equivalent read path), add a `decode_put_entry_binary` function that reverses the binary format:

```c
static int decode_put_entry_binary(uint8_t* data, size_t data_len,
                                    path_t** out_path, identifier_t** out_value,
                                    uint8_t chunk_size) {
    size_t pos = 0;

    // Read path_count
    if (pos + 2 > data_len) return -1;
    uint16_t path_count = read_be16(data + pos);
    pos += 2;

    // Read path_len (validation)
    if (pos + 4 > data_len) return -1;
    uint32_t path_len = read_be32(data + pos);
    pos += 4;

    // Build path from identifiers
    path_t* path = path_create(chunk_size);
    for (uint16_t i = 0; i < path_count; i++) {
        if (pos + 2 > data_len) { path_destroy(path); return -1; }
        uint16_t id_len = read_be16(data + pos);
        pos += 2;

        if (pos + id_len > data_len) { path_destroy(path); return -1; }
        buffer_t* ident_buf = buffer_create_from_existing_memory(data + pos, id_len);
        identifier_t* ident = identifier_create(ident_buf, chunk_size);
        buffer_destroy(ident_buf);
        path_append(path, ident);
        DEREFERENCE(ident, identifier_t);
        pos += id_len;
    }

    // Read value
    if (pos + 4 > data_len) { path_destroy(path); return -1; }
    uint32_t value_len = read_be32(data + pos);
    pos += 4;

    identifier_t* value = NULL;
    if (value_len > 0) {
        if (pos + value_len > data_len) { path_destroy(path); return -1; }
        buffer_t* val_buf = buffer_create_from_existing_memory(data + pos, id_len);
        value = identifier_create(val_buf, chunk_size);
        buffer_destroy(val_buf);
        pos += value_len;
    }

    *out_path = path;
    *out_value = value;
    return 0;
}
```

- [ ] **Step 5: Add WAL format version detection**

In the WAL read path, check the format version stored in the manifest or file header. If the entry was written in CBOR format, use the old `cbor_decode` path. If binary, use `decode_put_entry_binary`.

- [ ] **Step 6: Build and run tests**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build && cmake -DCMAKE_BUILD_TYPE=Release .. && make -j$(nproc) && ctest --output-on-failure`

Expected: All tests pass. New WAL entries are written in binary format.

- [ ] **Step 7: Add WAL round-trip test**

Add a test that writes entries in binary format, reads them back, and verifies the path and value match. Test with multi-identifier paths and binary values.

- [ ] **Step 8: Add WAL migration test**

Create a test that opens a CBOR-format WAL (from existing test fixtures or by writing one with the old code), reads entries, and writes new entries in binary format. Verify all entries are readable.

- [ ] **Step 9: Run valgrind leak check on WAL path**

Run: `valgrind --leak-check=full --error-exitcode=1 ./build/tests/test_database 2>&1 | tail -20`

Expected: Zero leaks in WAL read/write path.

- [ ] **Step 10: Commit**

```bash
git add src/Util/endian.h src/Database/wal.h src/Database/database.c src/Database/wal_manager.c
git commit -m "feat: add binary WAL format with big-endian encoding

Replace CBOR payload encoding with fixed binary format for paths
and values. All multi-byte integers use big-endian (network byte
order) for cross-platform portability. Legacy CBOR entries are
still readable. ~15% write improvement expected."
```

---

### Task 3: Batch WAL writev with One-Shot Timer

**Files:**
- Modify: `src/Database/wal_manager.h` (add buffer fields to `thread_wal_t`)
- Modify: `src/Database/wal_manager.c` (batch writev logic, timer callback, remove debouncer)
- Modify: `src/Database/database.c` (remove debouncer calls from write path)

- [ ] **Step 1: Add buffer fields to `thread_wal_t`**

Open `src/Database/wal_manager.h`. Add buffer fields to `thread_wal_t`:

```c
typedef struct {
    refcounter_t refcounter;
    PLATFORMLOCKTYPE(lock);
    uint64_t thread_id;
    char* file_path;
    int fd;
    wal_sync_mode_e sync_mode;
    hierarchical_timing_wheel_t* wheel;  // Timing wheel for one-shot flush
    transaction_id_t oldest_txn_id;
    transaction_id_t newest_txn_id;
    size_t current_size;
    size_t max_size;
    uint64_t pending_writes;

    // Batch write buffer
    uint8_t entry_buf[4096];     // Pre-allocated entry buffer
    size_t entry_buf_used;       // Bytes used in entry_buf
    uint8_t batch_count;         // Entries accumulated in current batch
    uint8_t batch_size;           // Max entries before flush (default: 4)
    int timer_active;             // 1 if one-shot timer is pending
    uint64_t timer_id;            // ID of the pending one-shot timer

    wal_manager_t* manager;
} thread_wal_t;
```

Remove `debouncer_t* fsync_debouncer` — replaced by the one-shot timer.

- [ ] **Step 2: Implement batch writev and one-shot timer in wal_manager.c**

Open `src/Database/wal_manager.c`. Add the timer callback and modify `thread_wal_write`:

```c
// One-shot timer callback: flush if buffer has entries
static void wal_flush_timer_callback(void* ctx) {
    thread_wal_t* twal = (thread_wal_t*)ctx;
    platform_lock(&twal->lock);
    if (twal->batch_count > 0) {
        thread_wal_flush_locked(twal);
    }
    twal->timer_active = 0;
    platform_unlock(&twal->lock);
}

// Flush all buffered entries (called under twal->lock)
static int thread_wal_flush_locked(thread_wal_t* twal) {
    if (twal->batch_count == 0 || twal->entry_buf_used == 0) return 0;

    // Write all buffered entries in a single writev call
    struct iovec iov;
    iov.iov_base = twal->entry_buf;
    iov.iov_len = twal->entry_buf_used;

    ssize_t written = writev(twal->fd, &iov, 1);
    if (written != (ssize_t)twal->entry_buf_used) {
        return -1;
    }

    twal->current_size += twal->entry_buf_used;

    // Handle sync modes
    if (twal->sync_mode == WAL_SYNC_IMMEDIATE) {
        fsync(twal->fd);
        twal->pending_writes = 0;
    }

    // Reset buffer
    twal->entry_buf_used = 0;
    twal->batch_count = 0;

    return 0;
}
```

Modify `thread_wal_write` to accumulate entries in the buffer instead of writing immediately:

```c
int thread_wal_write(thread_wal_t* twal, transaction_id_t txn_id,
                     wal_type_e type, buffer_t* data) {
    if (twal == NULL || data == NULL) return -1;

    // Build header (same 33-byte header as before)
    uint8_t header[33];
    size_t entry_size = 33 + data->size;

    // Check if entry fits in buffer
    if (twal->entry_buf_used + entry_size > sizeof(twal->entry_buf)) {
        // Buffer full — flush immediately
        platform_lock(&twal->lock);
        thread_wal_flush_locked(twal);
        platform_unlock(&twal->lock);
    }

    // Build header
    uint32_t crc = wal_crc32(data->data, data->size);
    header[0] = (uint8_t)type;
    transaction_id_serialize(&txn_id, header + 1);
    write_be32(header + 25, crc);
    write_be32(header + 29, (uint32_t)data->size);

    // Copy header + data to buffer (no lock needed — per-thread buffer)
    memcpy(twal->entry_buf + twal->entry_buf_used, header, 33);
    twal->entry_buf_used += 33;
    memcpy(twal->entry_buf + twal->entry_buf_used, data->data, data->size);
    twal->entry_buf_used += data->size;
    twal->batch_count++;

    // Check flush triggers
    if (twal->batch_count >= twal->batch_size) {
        // Buffer full — flush immediately
        platform_lock(&twal->lock);
        thread_wal_flush_locked(twal);
        platform_unlock(&twal->lock);
    } else if (twal->batch_count == 1) {
        // First entry — start one-shot timer (only for DEBOUNCED mode)
        if (twal->sync_mode == WAL_SYNC_DEBOUNCED && twal->wheel != NULL) {
            twal->timer_active = 1;
            twal->timer_id = hierarchical_timing_wheel_set_timer(
                twal->wheel, twal,
                wal_flush_timer_callback, NULL,
                (timer_duration_t){.milliseconds = twal->debounce_ms});
        }
    }
    // else: accumulating, timer already set or IMMEDIATE mode

    return 0;
}
```

For `WAL_SYNC_IMMEDIATE` mode, set `batch_size = 1` so every write flushes immediately.

- [ ] **Step 3: Remove debouncer calls from WAL write path**

Search for all `debouncer_debounce` and `debouncer_flush` calls in the WAL path and replace with the buffer-based flush:

Run: `grep -rn "debouncer_" src/Database/`

Remove `debouncer_t* fsync_debouncer` from `thread_wal_t` (replaced by timer fields).
Remove all `debouncer_debounce(twal->fsync_debouncer)` calls.
Remove all `debouncer_flush(twal->fsync_debouncer)` calls — replace with `thread_wal_flush_locked(twal)`.
Remove `debouncer_destroy` calls in `thread_wal_destroy`.

- [ ] **Step 4: Add `debounce_ms` field to `thread_wal_t`**

Add `uint64_t debounce_ms` to `thread_wal_t` for the one-shot timer delay. Initialize it from `wal_config.debounce_ms` in `create_thread_wal`.

- [ ] **Step 5: Handle WAL rotation with buffer**

When `thread_wal_write` detects the file is too large (current rotation check), flush the buffer first, then rotate:

```c
if (twal->current_size + entry_size > twal->max_size) {
    platform_lock(&twal->lock);
    thread_wal_flush_locked(twal);
    // Rotate WAL file
    thread_wal_rotate_locked(twal);
    platform_unlock(&twal->lock);
}
```

- [ ] **Step 6: Handle database_destroy and database_snapshot flush**

In `database_destroy` and `database_snapshot`, add calls to flush all thread WAL buffers:

```c
// Flush all thread WAL buffers before destroy
for (int i = 0; i < manager->thread_count; i++) {
    platform_lock(&manager->threads[i]->lock);
    thread_wal_flush_locked(manager->threads[i]);
    platform_unlock(&manager->threads[i]->lock);
}
```

- [ ] **Step 7: Build and run tests**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build && cmake -DCMAKE_BUILD_TYPE=Release .. && make -j$(nproc) && ctest --output-on-failure`

Expected: All tests pass.

- [ ] **Step 8: Add batch accumulation test**

Add a test that writes 3 entries, verifies they're not flushed yet (check file size hasn't changed), writes a 4th entry, verifies all 4 are flushed (check file size includes all 4 entries).

- [ ] **Step 9: Add timer flush test**

Add a test that writes 1 entry in DEBOUNCED mode, waits for the debounce window, and verifies the entry is flushed.

- [ ] **Step 10: Add IMMEDIATE mode test**

Add a test that writes entries in IMMEDIATE mode and verifies each write is flushed immediately (no buffering).

- [ ] **Step 11: Run valgrind leak check**

Run: `valgrind --leak-check=full --error-exitcode=1 ./build/tests/test_database 2>&1 | tail -20`

Expected: Zero leaks. No leaks in timer callback, buffer flush, or timer-no-op paths.

- [ ] **Step 12: Commit**

```bash
git add src/Database/wal_manager.h src/Database/wal_manager.c src/Database/database.c
git commit -m "feat: batch WAL writev with one-shot timer

Replace debouncer with per-thread write buffer and one-shot timer.
Entries accumulate in a thread-local buffer and flush when full
(4 entries by default) or after debounce_ms timeout. Timer is set
once per flush cycle and never cancelled — callback is idempotent.
Eliminates wheel->lock contention from per-write cancel+reschedule.
~10-15% write improvement expected."
```

---

### Task 4: Eliminate Path Deep Copy for LRU

**Files:**
- Modify: `src/HBTrie/path.h` (add `path_reference` declaration)
- Modify: `src/HBTrie/path.c` (add `path_reference` implementation)
- Modify: `src/Database/database_lru.h` (add `key_hash` and `entry_size` to `database_lru_node_t`)
- Modify: `src/Database/database_lru.c` (use `path_reference` instead of `path_copy`, hash-first comparison, cached entry size)

- [ ] **Step 1: Add `path_reference` to path.h and path.c**

Open `src/HBTrie/path.h`. Add declaration:

```c
path_t* path_reference(path_t* path);
```

Open `src/HBTrie/path.c`. Add implementation:

```c
path_t* path_reference(path_t* path) {
    if (path == NULL) return NULL;
    refcounter_reference((refcounter_t*)path);
    return path;
}
```

- [ ] **Step 2: Update `database_lru_node_t` in database_lru.h**

Open `src/Database/database_lru.h`. Add `key_hash` and change `path_t* path` semantics:

```c
struct database_lru_node_t {
    path_t* path;                   // Refcounted path (no deep copy)
    uint64_t key_hash;              // hash_path() result, compared first
    identifier_t* value;            // Value (reference counted)
    size_t memory_size;             // Cached entry size (not recomputed per put)
    database_lru_node_t* next;      // Next in LRU list (more recently used)
    database_lru_node_t* previous;  // Previous in LRU list (less recently used)
};
```

- [ ] **Step 3: Update `database_lru_cache_put` in database_lru.c**

Open `src/Database/database_lru.c`. Find `database_lru_cache_put`. Change the key storage from deep copy to reference counting:

Replace:
```c
path_t* stored_path = path_copy(path);  // Deep copy
```

With:
```c
path_t* stored_path = path_reference(path);  // Reference count
```

Also compute and store `key_hash`:

```c
uint64_t hash = hash_path(path);
node->key_hash = hash;
```

And compute `memory_size` once at insert time:

```c
node->memory_size = calculate_entry_memory(path, value);
```

Remove the `dup_path` hashmap key allocation function and replace with `path_reference`.

- [ ] **Step 4: Update hashmap comparison to hash-first**

In the hashmap configuration for the LRU cache, update the key comparison function to compare hashes first:

```c
static int lru_key_compare(const void* a, const void* b) {
    const database_lru_node_t* node_a = *(const database_lru_node_t**)a;
    const database_lru_node_t* node_b = *(const database_lru_node_t**)b;

    // Fast path: compare hashes first (O(1) integer comparison)
    if (node_a->key_hash != node_b->key_hash) return 1;

    // Slow path: hash collision, compare paths
    return path_compare(node_a->path, node_b->path);
}
```

Also update `hashmap_set_key_alloc_funcs` to use `path_reference` for key duplication and `path_destroy` for key freeing.

- [ ] **Step 5: Update `lru_node_destroy`**

Change `path_destroy` call to properly handle refcounted paths. Since the LRU now holds a reference, `path_destroy` decrements the refcount. If the path has no other references, it's freed. If it's still referenced elsewhere (e.g., by the caller), it stays alive.

This should work automatically since `path_destroy` already calls `refcounter_dereference` and frees when count reaches 0.

- [ ] **Step 6: Build and run tests**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/WaveDB/build && cmake -DCMAKE_BUILD_TYPE=Release .. && make -j$(nproc) && ctest --output-on-failure`

Expected: All tests pass.

- [ ] **Step 7: Add refcounted path lifecycle test**

Add a test that:
1. Creates a path, puts it in the LRU
2. Verifies the path's refcount incremented
3. Destroys the original path reference
4. Verifies the LRU entry is still valid (path not freed)
5. Evicts the LRU entry
6. Verifies the path is freed (no leaks)

- [ ] **Step 8: Run valgrind leak check**

Run: `valgrind --leak-check=full --error-exitcode=1 ./build/tests/test_database 2>&1 | tail -20`

Expected: Zero leaks. Path refcounts must be balanced — no leaks from LRU eviction, no double-frees when path is both in LRU and held by caller.

- [ ] **Step 9: Commit**

```bash
git add src/HBTrie/path.h src/HBTrie/path.c src/Database/database_lru.h src/Database/database_lru.c
git commit -m "feat: eliminate path deep copy for LRU cache

Use path_reference() instead of path_copy() for LRU cache keys.
Store hash_path() result for O(1) comparison before falling back
to path_compare(). Cache entry memory size instead of recomputing.
~10% single-threaded improvement expected."
```

---

### Task 5: Phase 2 Benchmark Comparison

- [ ] **Step 1: Run sync benchmark**

Run: `./build/tests/benchmark/benchmark_database_sync 2>&1`

Record put/get/delete/mixed throughput. Compare with Phase 1 results.

Expected: Put throughput should be significantly higher than Phase 1 (~100-115K ops/sec from ~80K).

- [ ] **Step 2: Run concurrent benchmark**

Run: `./build/tests/benchmark/benchmark_database 2>&1`

Record write/read/mixed throughput at 4, 8, 16, 32 threads.

- [ ] **Step 3: Record results and commit**

```bash
git add .benchmarks/
git commit -m "bench: update benchmark results after Phase 2 optimizations"
```