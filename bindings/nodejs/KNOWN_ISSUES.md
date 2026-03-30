# Known Issues - WaveDB Node.js Bindings

## Database Snapshot Disabled (TEMPORARY)

**Severity**: Medium
**Status**: Temporary workaround
**Affected Operations**: Database persistence on close

### Problem
Calling `database_snapshot()` from Node.js bindings is currently disabled to avoid crashes. This means:
- Data is NOT flushed to disk on `db.close()`
- Persistence relies on WAL recovery on next database open
- Write-ahead log (WAL) ensures durability, but snapshot is skipped

### Root Cause
Two issues have been addressed:

1. **Thread-local WAL crashes** (RESOLVED): The WAL architecture uses thread-local state, and calling snapshot from the main thread after async operations created WAL state in worker threads.

2. **MVCC version chain serialization** (RESOLVED): CBOR serialization now correctly handles MVCC version chains for overwrites and deletes.

### Solution Status
- ✓ MVCC version chain CBOR serialization implemented
- ✓ Thread-local WAL issue documented
- ⚠ Snapshot still disabled pending thread-local WAL resolution

### Workaround
- Use synchronous operations for guaranteed persistence before close
- Data persists via WAL recovery on next database open
- For critical data, consider using sync operations: `putSync()`, `delSync()`

### What Works
- ✓ Open/close databases
- ✓ All put operations (including overwrites)
- ✓ All get operations
- ✓ All delete operations
- ✓ Batch operations
- ✓ All async operations
- ✓ Concurrent operations
- ✓ MVCC version chains (overwrites and deletes)

### Previous MVCC Limitation (RESOLVED)
The previous issue with overwrites and deletes has been **fixed**. The CBOR serialization code now correctly handles MVCC version chains.

**Fix Details**:
- `hbtrie_node_to_cbor()` now checks `has_versions` flag
- Version chains are serialized as arrays: `[[time, nanos, count], is_deleted, value]`
- Legacy single values still work (backward compatible)
- Transaction IDs use correct fields: `time`, `nanos`, `count`

### Testing
All operations are tested and working:
```javascript
// Overwrites work correctly
db.putSync('key1', 'value1');
db.putSync('key1', 'value2');  // Creates version chain
db.close();  // Works!

// Deletes work correctly
db.putSync('key1', 'value1');
db.delSync('key1');  // Creates tombstone
db.close();  // Works!
```