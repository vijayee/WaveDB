# Thread-Local WAL Design Specification

**Date**: 2026-03-21
**Status**: Draft
**Author**: Claude (Design collaboration with user)

## Problem Statement

The current WAL implementation uses a single `current.wal` file protected by a lock. All threads compete for this lock when writing, creating contention that limits write throughput even with debounced fsync.

**Current throughput**: 10,000-100,000 ops/sec with debounced fsync
**Goal**: Eliminate write contention to achieve higher throughput with multiple threads

## Solution Overview

Replace the single contended WAL with thread-local WAL files coordinated by an append-only manifest:

- Each thread writes to its own file (`thread_<id>.wal`)
- Manifest tracks file states with atomic appends (no locks)
- Background compaction merges sealed files preserving global ordering
- Recovery reads all files and replays entries in transaction ID order

**Key benefits**:
- Zero lock contention on write path
- Strict global ordering preserved
- Configurable durability (IMMEDIATE/DEBOUNCED/ASYNC)
- Safe migration from legacy WAL with rollback support

## Architecture

### Components

```
wal_manager_t (global)
├── manifest_fd: File descriptor for manifest.dat
├── manifest_lock: Lock for compaction (not write path)
├── threads: Array of thread_wal_t*
└── compactor: Background compaction thread

thread_wal_t (per-thread)
├── fd: File descriptor for thread_<id>.wal
├── sync_mode: Durability mode
├── fsync_debouncer: For DEBOUNCED mode
└── file metadata: path, size, transaction IDs
```

### File Formats

**Thread-local WAL** (`thread_<id>.wal`):
```
[Entry 1: type(1B) + txn_id(24B) + crc32(4B) + len(4B) + data(NB)]
[Entry 2: ...]
...
```

**Manifest** (`manifest.dat`):
```
[Header: version(4B) + migration_state(4B) + timestamp(8B)]
[Entry 1: thread_id(8B) + file_path(256B) + status(1B) + newest_txn(24B)]
[Entry 2: ...]
...
```

Entry statuses:
- `ACTIVE` (0x01): File is being written to
- `SEALED` (0x02): File complete, ready for compaction
- `COMPACTED` (0x03): File merged, can be deleted

### Write Path

```c
int thread_wal_write(thread_wal_t* twal, transaction_id_t txn_id,
                     wal_type_e type, buffer_t* data) {
    // 1. Write to thread-local file (no lock)
    write(twal->fd, header, 33);
    write(twal->fd, data->data, data->size);

    // 2. Sync based on durability mode
    if (twal->sync_mode == WAL_SYNC_IMMEDIATE) {
        fsync(twal->fd);
    } else if (twal->sync_mode == WAL_SYNC_DEBOUNCED) {
        debouncer_debounce(twal->fsync_debouncer);
    }

    // 3. Update manifest (atomic append, no lock)
    write_manifest_entry(manager->manifest_fd, twal->thread_id,
                         twal->file_path, ACTIVE, twal->newest_txn_id);

    return 0;
}
```

**Key point**: No locks held during write. Manifest append is atomic via `O_APPEND` flag.

### Recovery Path

```c
int wal_manager_recover(wal_manager_t* manager) {
    // 1. Read manifest entries
    read_manifest(manager->manifest_path, &entries, &count);

    // 2. Scan directory for any thread_*.wal files not in manifest
    scan_wal_directory(manager->location, &wal_files, &wal_count);

    // 3. Combine sources (manifest + directory scan)
    // Handles case where manifest fsync was delayed

    // 4. Read all entries from all files
    for each file in combined_list {
        read_wal_file(file, &all_entries, &entry_count);
    }

    // 5. Sort by transaction ID (timestamp + sequence + thread)
    qsort(all_entries, entry_count, sizeof(wal_entry_t),
          compare_transaction_ids);

    // 6. Replay in order
    for (size_t i = 0; i < entry_count; i++) {
        apply_wal_entry(db, &all_entries[i]);
    }

    return 0;
}
```

### Compaction Strategy

**Triggers**:
- Idle time: 10+ seconds with no writes
- Interval: Every 60 seconds maximum
- Size: When sealed files exceed threshold

**Process**:
```c
int compact_wal_files(wal_manager_t* manager) {
    // 1. Find all SEALED files
    find_sealed_files(manager, &sealed_files, &sealed_count);

    // 2. Read all entries from sealed files
    for each file in sealed_files {
        read_wal_file(file, &entries, &entry_count);
    }

    // 3. Sort entries by transaction ID
    qsort(entries, entry_count, sizeof(wal_entry_t),
          compare_transaction_ids);

    // 4. Write to compacted_<seq>.wal
    create_compacted_file(manager, entries, entry_count);

    // 5. Update manifest (mark sealed files as COMPACTED)
    append_manifest_entry(manager, sealed_files, COMPACTED);
    append_manifest_entry(manager, compacted_path, SEALED);

    // 6. Delete old sealed files
    for each sealed_file {
        unlink(sealed_file);
    }

    return 0;
}
```

### Sealing Files

Files transition to SEALED state when:

1. **Thread exits**: Seal file on cleanup
2. **File reaches max_size**: Seal current, create new file for thread
3. **Manual seal**: Database API allows explicit sealing

## Durability Configuration

### Configuration Structure

```c
typedef struct {
    wal_sync_mode_e sync_mode;      // IMMEDIATE, DEBOUNCED, ASYNC
    uint64_t debounce_ms;           // Debounce window (default 100ms)
    uint64_t idle_threshold_ms;     // Compaction idle trigger (default 10s)
    uint64_t compact_interval_ms;   // Compaction interval (default 60s)
    size_t max_file_size;          // Max file size before seal (default 128KB)
} wal_config_t;
```

### Durability Modes

**IMMEDIATE (safest)**:
- Thread file: `write() + fsync()` on every entry
- Manifest: `write()` with debounced fsync
- Durability: Entry survives crash immediately after write

**DEBOUNCED (balanced)**:
- Thread file: `write()` with debounced fsync (100ms)
- Manifest: `write()` with debounced fsync
- Durability: Entry survives crash within debounce window

**ASYNC (fastest)**:
- Thread file: `write()` only (no fsync)
- Manifest: `write()` only (no fsync)
- Durability: Best-effort, may lose recent writes on crash

### Thread-Local Storage

```c
static __thread thread_wal_t* thread_local_wal = NULL;

thread_wal_t* get_thread_wal(wal_manager_t* manager) {
    if (thread_local_wal == NULL) {
        uint64_t thread_id = get_current_thread_id();
        thread_local_wal = create_thread_wal(manager, thread_id);
    }
    return thread_local_wal;
}
```

## Error Handling

### Thread-Local WAL Errors

- **Write failure**: Log error, return failure, other threads continue
- **Fsync failure**: Log error, mark thread WAL as unhealthy
- **Manifest write failure**: Log warning, recovery will find via directory scan

### Recovery Error Handling

- **Missing file in manifest**: Log warning, continue with other files
- **Corrupted entry (CRC mismatch)**: Stop reading that file, continue with others
- **Partial manifest entry**: Stop reading manifest, use directory scan
- **No manifest**: Scan directory for all `thread_*.wal` files

**Principle**: Never fail recovery if valid data exists. Process what can be read, log warnings for inconsistencies.

### Manifest Corruption

Manifest is optimization, not correctness requirement:
- If manifest corrupted/missing, recovery scans directory
- Slightly slower recovery, but no data loss
- Manifest can be rebuilt from WAL files

## Migration Strategy

### Migration States

```c
typedef enum {
    MIGRATION_NONE,          // Fresh database
    MIGRATION_PENDING,       // Legacy detected, needs migration
    MIGRATION_IN_PROGRESS,   // Migration started but not completed
    MIGRATION_COMPLETE,      // Successfully migrated
    MIGRATION_FAILED         // Migration failed, can rollback
} migration_state_e;
```

### Recovery Options

```c
typedef struct {
    int force_legacy;           // Force use of legacy WAL
    int force_migration;        // Force re-migration
    int rollback_on_failure;    // Auto-rollback if failed
    int keep_backup;            // Keep backup after migration
} wal_recovery_options_t;
```

### Migration Process

1. **Detection**: Check for `current.wal` (legacy) vs `manifest.dat` (new)
2. **Backup**: Rename `current.wal` to `current.wal.backup`
3. **Create manifest**: Write `MIGRATION_IN_PROGRESS` state
4. **Migrate entries**: Read from backup, write to thread-local WAL (thread ID 0)
5. **Seal and compact**: Compact migration WAL immediately
6. **Complete**: Update manifest to `MIGRATION_COMPLETE`
7. **Cleanup**: Optionally delete backup

### Rollback

If migration fails or issues detected:
```c
int rollback_to_legacy(const char* location, wal_config_t* config) {
    // 1. Restore backup
    rename("current.wal.backup", "current.wal");

    // 2. Remove manifest and thread files
    unlink("manifest.dat");
    for each thread_*.wal {
        unlink(file);
    }

    // 3. Load legacy WAL
    return wal_load(location, config);
}
```

## Implementation Plan

### Phase 1: Core Infrastructure

1. Create `wal_manager_t` and `thread_wal_t` structures
2. Implement thread-local WAL creation and destruction
3. Implement manifest file format and append logic
4. Add manifest header with migration state tracking

### Phase 2: Write Path

1. Implement `thread_wal_write()` with no-lock design
2. Add thread-local storage for per-thread WAL
3. Implement atomic manifest appends with `O_APPEND`
4. Add debounced manifest fsync

### Phase 3: Recovery

1. Implement manifest reading with error handling
2. Implement directory scanning for WAL files
3. Implement entry merging and sorting by transaction ID
4. Implement replay logic

### Phase 4: Compaction

1. Create background compaction thread
2. Implement sealed file detection
3. Implement merge and compaction logic
4. Implement idle-time detection for compaction triggers

### Phase 5: Migration

1. Implement legacy WAL detection
2. Implement migration process with state tracking
3. Implement rollback support
4. Add recovery options for manual control

### Phase 6: Testing

1. Unit tests for thread-local WAL operations
2. Unit tests for manifest operations
3. Integration tests for recovery
4. Stress tests for concurrent writes
5. Migration tests (legacy → new, rollback scenarios)
6. Performance benchmarks comparing old vs new

## Performance Expectations

### Write Throughput

**Current (single WAL)**:
- IMMEDIATE: ~1,000 ops/sec (fsync bottleneck)
- DEBOUNCED: ~10,000-100,000 ops/sec (lock contention)
- ASYNC: ~100,000+ ops/sec (lock contention)

**New (thread-local WAL)**:
- IMMEDIATE: ~1,000 ops/sec per thread (fsync bottleneck, no contention)
- DEBOUNCED: ~10,000-100,000 ops/sec per thread (no contention)
- ASYNC: ~1,000,000+ ops/sec (no contention)

**With N threads**: Approximately N× throughput improvement until disk I/O bottleneck.

### Lock Contention

**Current**: Single lock for all writes
**New**: No locks on write path, lock only during compaction (background)

### Recovery Time

**Current**: O(entries) - read single file, replay in order
**New**: O(entries log entries) - read multiple files, merge sort, replay
- Slightly slower due to merge sort
- Offset by faster write throughput during normal operation
- Manifest optimization reduces directory scan time

## Risks and Mitigations

### Risk: Manifest Corruption

**Mitigation**: Manifest is optimization, not correctness requirement
- Recovery scans directory if manifest missing/corrupted
- Slower recovery, no data loss

### Risk: Many Small Files

**Mitigation**: Background compaction merges sealed files
- Configurable compaction interval
- Idle-time trigger prevents unbounded growth

### Risk: Migration Issues

**Mitigation**: Safe migration with rollback support
- Backup preserved until explicit cleanup
- Migration state tracked in manifest
- Manual control via recovery options

### Risk: Transaction Ordering Across Threads

**Mitigation**: Transaction IDs encode global ordering (timestamp + sequence)
- Compaction merges by transaction ID
- Recovery replays in global order
- Strict ordering guaranteed

## Open Questions

None. Design approved by user.

## Implementation Notes

- Use `O_APPEND` flag for manifest writes to ensure atomicity
- Manifest entries must be < `PIPE_BUF` (typically 4KB) for atomic appends
- Thread ID should be unique per thread (use pthread_self or similar)
- Compaction thread should check for writes before starting (avoid racing with active threads)
- Consider using `O_DIRECT` for WAL files if disk alignment requirements are met

## References

- Current WAL implementation: `src/Database/wal.h`, `src/Database/wal.c`
- Transaction ID: `src/Workers/transaction_id.h`
- Debouncer: `src/Time/debouncer.h`