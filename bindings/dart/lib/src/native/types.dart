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

/// Opaque handle to an encrypted database configuration
/// Maps to encrypted_database_config_t in C
base class encrypted_database_config_t extends Opaque {}

/// Opaque handle to a Graph layer
/// Maps to graph_layer_t in C
base class graph_layer_t extends Opaque {}

/// Opaque handle to a Graph layer configuration
/// Maps to graph_layer_config_t in C
base class graph_layer_config_t extends Opaque {}

/// Opaque handle to a database subtree
/// Maps to database_subtree_t in C
base class database_subtree_t extends Opaque {}

/// Opaque handle to a Graph result (array of vertex strings)
/// Maps to graph_result_t in C
base class graph_result_t extends Opaque {}

/// Parse error struct for graph DSL parsing
/// Maps to graph_parse_error_t in C: { int ok; int position; char message[256]; }
base class GraphParseError extends Struct {
  @Int32()
  external int ok;

  @Int32()
  external int position;

  @Array(256)
  external Array<Uint8> message;
}

/// WAL sync mode enumeration
/// Maps to wal_sync_mode_e in C
enum WalSyncMode {
  immediate(0),
  debounced(1),
  async(2);

  final int value;
  const WalSyncMode(this.value);
}

/// Vacuum / compaction mode.
/// Maps to vacuum_mode_t in C: VACUUM_MODE_MANUAL_ONLY=0,
/// VACUUM_MODE_STRICT=1, VACUUM_MODE_ADAPTIVE=2.
enum VacuumMode {
  /// Background vacuum disabled; vacuum only runs when explicitly requested
  /// via [WaveDB.vacuum].
  manualOnly(0),
  /// Auto-trigger vacuum when the configured stale thresholds are exceeded.
  strict(1),
  /// Like strict, but defers vacuum during periods of high write activity
  /// (measured against [VacuumConfig.adaptiveBusyThreshold]).
  adaptive(2);

  final int value;
  const VacuumMode(this.value);
}

/// Vacuum / compaction configuration.
///
/// Maps to vacuum_config_t in C. All fields default to the C defaults
/// (see database_config_default in database_config.c). Pass to [WaveDB]
/// via [WaveDBConfig.vacuumConfig].
class VacuumConfig {
  /// When to trigger vacuum passes.
  final VacuumMode mode;

  /// Fraction of the page file that must be stale before strict/adaptive
  /// auto-vacuum fires (0.0–1.0). Default 0.30.
  final double staleThreshold;

  /// Minimum total page file size in bytes before auto-vacuum is considered.
  /// Below this, the file is too small to be worth compacting. Default 64 MiB.
  final int minFileSizeBytes;

  /// Minimum stale bytes before auto-vacuum is considered. Default 16 MiB.
  final int minStaleBytes;

  /// Time to wait for in-flight writes to drain before vacuum starts, in ms.
  /// Default 5000.
  final int drainTimeoutMs;

  /// Time to wait for open cursors to close before vacuum gives up, in ms.
  /// Default 60000.
  final int cursorCloseWaitMs;

  /// Maximum wall-clock runtime for a single vacuum pass, in ms. Default 30000.
  final int maxRuntimeMs;

  /// How long writers block waiting for vacuum to finish before failing the
  /// write, in ms. 0 = block indefinitely. Default 0.
  final int writerBlockTimeoutMs;

  /// Adaptive-mode threshold above which vacuum is deferred due to busy
  /// writers. Default 32.
  final int adaptiveBusyThreshold;

  const VacuumConfig({
    this.mode = VacuumMode.strict,
    this.staleThreshold = 0.30,
    this.minFileSizeBytes = 64 * 1024 * 1024,
    this.minStaleBytes = 16 * 1024 * 1024,
    this.drainTimeoutMs = 5000,
    this.cursorCloseWaitMs = 60000,
    this.maxRuntimeMs = 30000,
    this.writerBlockTimeoutMs = 0,
    this.adaptiveBusyThreshold = 32,
  });
}

/// Vacuum introspection snapshot — same signals the auto-triggers evaluate.
///
/// Maps to vacuum_status_t in C. Returned by [WaveDB.vacuumStatus]. Useful
/// in MANUAL_ONLY mode where the caller polls and decides when to call
/// [WaveDB.vacuum].
class VacuumStatus {
  /// Current page file size in bytes.
  final int fileSize;

  /// Total stale region bytes in the page file.
  final int staleBytes;

  /// stale_bytes / file_size (0.0 if file_size == 0).
  final double staleRatio;

  /// True while a vacuum pass is running.
  final bool vacuumInProgress;

  /// Number of currently-open cursors (vacuum blocks if > 0).
  final int openCursorCount;

  /// True if the auto-trigger predicate is satisfied
  /// (stale_ratio >= stale_threshold AND file_size >= min_file_size_bytes
  /// AND stale_bytes >= min_stale_bytes).
  final bool wouldTrigger;

  const VacuumStatus({
    required this.fileSize,
    required this.staleBytes,
    required this.staleRatio,
    required this.vacuumInProgress,
    required this.openCursorCount,
    required this.wouldTrigger,
  });

  @override
  String toString() =>
      'VacuumStatus(fileSize=$fileSize, staleBytes=$staleBytes, '
      'staleRatio=$staleRatio, vacuumInProgress=$vacuumInProgress, '
      'openCursorCount=$openCursorCount, wouldTrigger=$wouldTrigger)';
}

/// Buffer structure for raw data
/// Maps to buffer_t in C
///
/// C layout on 64-bit Linux/macOS:
/// - _Atomic uint_fast16_t count (8 bytes, uint_fast16_t = unsigned long)
/// - _Atomic uint_fast8_t yield (1 byte + 7 padding)
/// - uint8_t* data (8 bytes)
/// - size_t size (8 bytes)
/// Total: 32 bytes
///
/// WARNING: On 64-bit Windows, uint_fast16_t is 2 bytes (not 8), so
/// refcounter_t is 4 bytes and this layout is wrong. Windows support
/// requires a platform-specific layout.
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
/// Layout: _Atomic uint_fast16_t count (8 bytes) + _Atomic uint_fast8_t yield (1 byte + 7 padding) = 16 bytes
base class refcounter_t extends Struct {
  @Array(16)
  external Array<Uint8> _refcounter;
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

/// Raw batch operation (input)
/// Maps to raw_op_t in C
base class RawOp extends Struct {
  external Pointer<Uint8> key;
  @Size()
  external int keyLen;
  external Pointer<Uint8> value;
  @Size()
  external int valueLen;
  @Int32()
  external int type; // 0 = put, 1 = delete
}

/// Raw scan result (output)
/// Maps to raw_result_t in C
base class RawResult extends Struct {
  external Pointer<Uint8> key;
  @Size()
  external int keyLen;
  external Pointer<Uint8> value;
  @Size()
  external int valueLen;
}