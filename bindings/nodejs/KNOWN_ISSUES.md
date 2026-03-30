# Known Issues - WaveDB Node.js Bindings

## Database Snapshot Disabled (TEMPORARY)

**Severity**: Medium
**Status**: Temporary workaround - WAL recovery provides persistence
**Affected Operations**: Database persistence on close

### Problem
Calling `database_snapshot()` from Node.js bindings is currently disabled due to a crash when computing hash for MVCC version chains.

### Root Cause
1. **Thread-local WAL** (RESOLVED): The WAL manager correctly flushes all thread-local WALs before snapshot
2. **MVCC serialization** (RESOLVED): CBOR serialization/deserialization of version chains works correctly
3. **Hash computation** (ISSUE): `hbtrie_compute_hash()` calls `hbtrie_to_cbor()` which serializes version chains, but there's a crash during hash computation

### Current Behavior
- Data **persists via WAL recovery** on next database open
- Synchronous operations provide durability within session
- Close/reopen works for simple data (no version chains)
- Crash occurs when closing database after overwrites/deletes

### Workaround
```javascript
// Use synchronous operations for durability
db.putSync('key', 'value');
db.close();

// Reopen - data recovered from WAL
db = new WaveDB(path);
const value = db.getSync('key');  // Value is available
```

### What Works
- ✅ Simple writes/reads without version chains
- ✅ Data persists via WAL recovery
- ✅ All operations work in-memory during session
- ✅ Overwrites and deletes work (no crash until close)

### What Doesn't Work
- ✗ Snapshot with MVCC version chains (crashes on close after overwrites/deletes)
- ⚠️ Persistence for version chains relies on WAL recovery

### Resolution Plan
1. Debug `hbtrie_compute_hash()` crash with version chains
2. Consider alternative hash computation that doesn't serialize
3. Or implement separate serialization path for hash computation
4. Re-enable `database_snapshot()` once resolved