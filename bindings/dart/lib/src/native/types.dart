// lib/src/native/types.dart
import 'dart:ffi';

/// Opaque handle to a WaveDB database
/// Maps to database_t in C
base class database_t extends Opaque {}

/// Opaque handle to a path (sequence of identifiers)
/// Maps to path_t in C
base class path_t extends Opaque {}

/// Opaque handle to an identifier (value or key)
/// Maps to identifier_t in C
base class identifier_t extends Opaque {}

/// Opaque handle to a database iterator
/// Maps to database_iterator_t in C
base class database_iterator_t extends Opaque {}

/// Opaque handle to a database configuration
/// Maps to database_config_t in C
base class database_config_t extends Opaque {}

/// WAL sync mode enumeration
/// Maps to wal_sync_mode_e in C
enum WalSyncMode {
  immediate(0),
  debounced(1),
  async(2);

  final int value;
  const WalSyncMode(this.value);
}

/// Buffer structure for raw data
/// Maps to buffer_t in C
///
/// C layout on 64-bit Linux:
/// - atomic_uint_fast16_t count (8 bytes, aligned)
/// - atomic_uint_fast8_t yield (1 byte + 7 padding)
/// - uint8_t* data (8 bytes)
/// - size_t size (8 bytes)
/// Total: 32 bytes
base class buffer_t extends Struct {
  // refcounter_t: 16 bytes (atomic count + yield)
  @Array(16)
  external Array<Uint8> _refcounter;

  // Data pointer at offset 16
  external Pointer<Uint8> data;

  // Size at offset 24
  @UintPtr()
  external int size;
}

/// Reference counter structure (first field of refcounted structs)
/// Maps to refcounter_t in C
/// Note: Uses C11 _Atomic for lock-free reference counting.
base class refcounter_t extends Struct {
  @Uint16()
  external int count;

  @Uint8()
  external int yield;
}

/// Opaque handle to a GraphQL layer
/// Maps to graphql_layer_t in C
base class graphql_layer_t extends Opaque {}

/// Opaque handle to a GraphQL layer configuration
/// Maps to graphql_layer_config_t in C
base class graphql_layer_config_t extends Opaque {}

/// Opaque handle to a GraphQL result
/// Maps to graphql_result_t in C
base class graphql_result_t extends Opaque {}

/// Opaque handle to a GraphQL result node
/// Maps to graphql_result_node_t in C
base class graphql_result_node_t extends Opaque {}

/// Opaque handle to a C promise
/// Maps to promise_t in C
base class promise_t extends Opaque {}

/// Opaque handle to a C async error
/// Maps to async_error_t in C
base class async_error_t extends Opaque {}

/// Opaque handle to a write batch
/// Maps to batch_t in C
base class batch_t extends Opaque {}