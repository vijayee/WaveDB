# MVCC Version Chain Fix - Summary

## Problem

The WaveDB Node.js bindings crashed when closing databases after creating MVCC version chains (overwrites or deletes). The crash occurred in `cbor_incref()` during CBOR serialization of the trie structure.

## Root Cause

The CBOR serialization code in `hbtrie_node_to_cbor()` was written before MVCC version chains were implemented. It assumed `entry->value` always contained an `identifier_t*`, but with MVCC:

1. The `bnode_entry_t` union uses either `value` (legacy) or `versions` (MVCC)
2. The `has_versions` flag indicates which field is active
3. When `has_versions == 1`, the entry has a version chain, not a single value

The old code didn't check `has_versions` and tried to serialize `entry->value` as an `identifier_t*`, which was actually a `version_entry_t*`, causing memory corruption.

## Fix

### 1. Updated `hbtrie_node_to_cbor()` in `src/HBTrie/hbtrie.c`

Added MVCC version chain serialization:

```c
if (entry->has_versions) {
    // MVCC: Serialize version chain
    // Count versions
    size_t version_count = 0;
    version_entry_t* current = entry->versions;
    while (current != NULL) {
        version_count++;
        current = current->next;
    }

    // Create array of versions: [[time, nanos, count], is_deleted, value]
    cbor_item_t* versions_array = cbor_new_definite_array(version_count);

    // Serialize each version...
} else {
    // Legacy: Single value
    cbor_item_t* value_cbor = identifier_to_cbor(entry->value);
    cbor_array_push(entry_item, value_cbor);
    cbor_decref(&value_cbor);
}
```

### 2. Updated `cbor_to_hbtrie_node()` in `src/HBTrie/hbtrie.c`

Added MVCC version chain deserialization:

```c
if (entry.has_value) {
    if (cbor_isa_array(value_or_child)) {
        // MVCC: Deserialize version chain
        entry.has_versions = 1;
        // Deserialize each version...
    } else {
        // Legacy: Single value
        entry.has_versions = 0;
        entry.value = cbor_to_identifier(value_or_child, DEFAULT_CHUNK_SIZE);
    }
}
```

### 3. Corrected Transaction ID Serialization

The `transaction_id_t` structure has fields:
- `uint64_t time`
- `uint64_t nanos`
- `uint64_t count`

Serialized as: `[time, nanos, count]`

### 4. Disabled Database Snapshot

Disabled `database_snapshot()` in `database.cc` to avoid:
1. Thread-local WAL state issues
2. Need for snapshot during close (persistence relies on WAL recovery)

## Serialization Format

### Legacy Single Value
```cbor
[key_bstr, has_value, identifier_bstr]
```

### MVCC Version Chain
```cbor
[key_bstr, has_value, versions_array]
```

Where `versions_array` is:
```cbor
[
  [[time, nanos, count], is_deleted, value_or_null],
  [[time, nanos, count], is_deleted, value_or_null],
  ...
]
```

## Testing

All operations now work correctly:
- ✓ Simple overwrites
- ✓ Multiple overwrites to same key
- ✓ Delete operations
- ✓ Overwrite then delete
- ✓ Async operations with overwrites
- ✓ Async operations with deletes
- ✓ Mixed operations
- ✓ Batch operations with overwrites
- ✓ Multiple keys with overwrites
- ✓ Database close and reopen

## Files Modified

1. `src/HBTrie/hbtrie.c`
   - `hbtrie_node_to_cbor()` - Added MVCC serialization
   - `cbor_to_hbtrie_node()` - Added MVCC deserialization

2. `bindings/nodejs/src/database.cc`
   - `Close()` - Disabled `database_snapshot()`

3. `bindings/nodejs/KNOWN_ISSUES.md`
   - Updated to reflect MVCC fix
   - Documented snapshot workaround

## Backward Compatibility

The serialization is backward compatible:
- Legacy entries (single value) deserialize correctly
- MVCC entries (version chains) use array format
- Deserialization checks if value is array or bytestring to determine format

## Future Work

1. Re-enable `database_snapshot()` after resolving thread-local WAL issues
2. Implement WAL compaction to manage version chain growth
3. Add MVCC garbage collection to remove old versions