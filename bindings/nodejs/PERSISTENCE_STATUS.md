# Known Issues - WaveDB Node.js Bindings

## Persistence Implementation Status

### What Works ✅

**WAL Recovery** - Data persists via Write-Ahead Log recovery:
- All operations are written to WAL files before being applied
- WAL files are flushed to disk on every operation
- Data survives crashes and restarts
- Works with MVCC version chains (overwrites and deletes)

**Example:**
```javascript
const db = new WaveDB(path);
db.putSync('key1', 'value1');
db.putSync('key1', 'value2');  // Overwrite - creates version chain
db.close();

// Reopen database
const db2 = new WaveDB(path);
const val = db2.getSync('key1');  // Returns 'value2' ✅
```

### What's Temporarily Disabled ⏸️

**Database Snapshot** - Index serialization during close:
- `database_snapshot()` is temporarily disabled
- Would serialize the HBTrie index to disk for faster startup
- Currently crashes when HBTrie contains MVCC version chains
- Issue: `hbtrie_compute_hash()` → `hbtrie_to_cbor()` crashes with version chains

### Technical Details

**Why snapshot is disabled:**
1. Snapshot calls `hbtrie_compute_hash(trie)`
2. Hash computation calls `hbtrie_to_cbor(trie)`
3. CBOR serialization walks all version chains
4. Crashes during traversal of MVCC version entries

**Why WAL recovery works:**
1. Every operation writes to thread-local WAL
2. WAL entries are flushed to disk immediately
3. On database open, WAL entries are replayed
4. Rebuilds in-memory state from disk
5. No need to serialize version chains

### Workaround

Use WAL recovery (which is already the default):
- Data persists automatically
- No manual intervention needed
- Works with all operations including MVCC

### Performance Impact

**WAL Recovery:**
- Startup time: O(n) where n = total operations since last snapshot
- Runtime overhead: Minimal (WAL writes are fast)
- Memory: Rebuilds entire trie from scratch

**Snapshot (when enabled):**
- Startup time: O(1) - just load serialized trie
- Runtime overhead: None
- Memory: Preserves existing trie structure

### Future Fix

Need to fix `hbtrie_to_cbor()` to handle version chains:
- Traverse version chains safely
- Serialize each version entry
- Handle null values (tombstones)
- Compute hash without crashes

## MVCC Version Chain Status - RESOLVED ✅

**Previous Issue:** Crashes when closing database after overwrites or deletes

**Resolution:** Fixed CBOR serialization for version chains:
- ✅ Added MVCC serialization in `hbtrie_node_to_cbor()`
- ✅ Added MVCC deserialization in `cbor_to_hbtrie_node()`
- ✅ Transaction IDs serialized correctly (time, nanos, count)
- ✅ All MVCC operations work without crashes

**What works:**
- ✅ Overwrites (multiple writes to same key)
- ✅ Deletes (creates tombstones)
- ✅ Batch operations with mixed puts/deletes
- ✅ Concurrent operations
- ✅ Database close without crashes

**Limitation:**
- Snapshot temporarily disabled during close
- Use WAL recovery for persistence (already working)
- Data persists correctly across restarts