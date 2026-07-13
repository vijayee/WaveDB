// lib/src/vector_layer.dart
//
// VectorLayer: approximate-nearest-neighbour index on top of WaveDB.
//
// Mirrors graph_layer.dart's wrapper pattern: hold the opaque ffi pointer,
// call lookupFunction via WaveDBNative, encode strings/bytes, check rc, map
// errors, lifecycle via dispose() + NativeFinalizer.
//
// Config is split into a Format tier (immutable after construct) and a
// Runtime tier (freely mutable via reconfigure). The wrapper exposes these
// as `VectorFormat` and `VectorRuntime` classes; `_fillFormat`/`_fillRuntime`
// copy them into the C structs.
//
// Only the sync API is wired here (insert/search/delete/train/rebuild/count/
// reconfigure). The async variants take a caller-owned `promise_t*` whose
// resolve/reject callbacks would need a NativeCallable.listener trampoline
// (the graph_layer.dart pattern). Declared in wavedb_bindings.dart so the
// symbols resolve, but VectorLayer does not call them — deferred for a
// follow-up. The sync API is the priority.
import 'dart:ffi';
import 'dart:typed_data';
import 'package:ffi/ffi.dart';

import 'native/types.dart';
import 'native/wavedb_bindings.dart';
import 'exceptions.dart';
import 'subtree.dart';
import 'database.dart';

/// C `vl_index_type_t` enum values.
class IndexType {
  const IndexType._();
  static const int flat = 0; // exact brute-force; baseline + IVF cold-start
  static const int ivf = 1; // inverted file
  static const int slsh = 2; // SK-LSH bidirectional scan
}

/// C `vl_distance_t` enum values.
class Distance {
  const Distance._();
  static const int l2 = 0;
  static const int cosine = 1; // default for embedding workloads
  static const int dot = 2;
}

/// Immutable format tier — set at create time, cannot be changed after.
///
/// To change any field here, drop the layer (and its subtree / separate db)
/// and recreate. `vector_layer_reconfigure` accepts a [VectorRuntime] only;
/// the C signature makes format mutation structurally impossible.
class VectorFormat {
  final int indexType;
  final int dim;
  final String delimiter;
  final int distance;
  final int ivfNClusters;
  final int slshLshTables;
  final int slshHashBits;
  final double slshBucketWidth;

  const VectorFormat({
    required this.indexType,
    required this.dim,
    this.delimiter = '/',
    this.distance = Distance.cosine,
    this.ivfNClusters = 0,
    this.slshLshTables = 0,
    this.slshHashBits = 0,
    this.slshBucketWidth = 0.0,
  });
}

/// Runtime tier — freely mutable via [VectorLayer.reconfigure].
class VectorRuntime {
  final int topK;
  final int syncOnly;
  final int ivfNprobe;
  final int ivfFlatUntil;
  final int slshScanRadius;

  const VectorRuntime({
    this.topK = 10,
    this.syncOnly = 1,
    this.ivfNprobe = 8,
    this.ivfFlatUntil = 1000,
    this.slshScanRadius = 200,
  });
}

/// One hit from [VectorLayer.searchSync].
///
/// `id` is the raw string passed to [VectorLayer.insertSync]. `metadata` is
/// the raw bytes passed to `insertSync` (empty `Uint8List` if none).
/// `distance` is the metric distance (lower = closer for L2; for cosine/dot
/// the C layer reports the post-transform value — see
/// src/Layers/vector/vector_distance.c).
class VectorResult {
  final String id;
  final double distance;
  final Uint8List metadata;

  const VectorResult(this.id, this.distance, this.metadata);

  String get metadataAsString =>
      String.fromCharCodes(metadata);

  @override
  String toString() =>
      'VectorResult(id=$id, distance=${distance.toStringAsFixed(4)}, '
      'metadata=${metadata.length} bytes)';
}

/// Exception thrown by vector layer operations.
class VectorLayerException implements Exception {
  final String message;
  const VectorLayerException(this.message);

  @override
  String toString() => 'VectorLayerException: $message';
}

/// ANN index layered on a WaveDB database (shared) or a dedicated db.
///
/// Two construction modes:
///
/// - [VectorLayer.openSeparate] — dedicated database at `dbLocation`. Clean
///   LRU/WAL tuning, no key-space sharing. Mirrors the Hippo document_db
///   split.
///
/// - [VectorLayer.open] — shares `db`'s underlying database. If `subtree`
///   is supplied, all keys land under that subtree prefix; otherwise keys
///   land directly in `db` under `{indexName}/...`.
class VectorLayer implements Finalizable {
  Pointer<vector_layer_t>? _vl;
  bool _closed = false;
  // Keep a reference to the parent db/subtree so they aren't GC'd while
  // the layer is alive. The C vector_layer_t does NOT hold a refcount on
  // the db/subtree, so the Dart side must. Accessed via [dbRef]/[subtreeRef]
  // below to keep the analyzer from flagging these as unused.
  final WaveDB? _db;
  final Subtree? _subtree;

  /// Reference to the parent database (for shared-db mode); null when opened
  /// via [openSeparate]. Held to prevent the parent [WaveDB] from being
  /// garbage-collected while this layer is alive.
  WaveDB? get dbRef => _db;

  /// Reference to the parent subtree, if any. Held to prevent GC of the
  /// [Subtree] while this layer is alive.
  Subtree? get subtreeRef => _subtree;

  static final NativeFinalizer _finalizer = NativeFinalizer(
    WaveDBNative.vectorLayerDestroyNative.cast(),
  );

  VectorLayer._(this._vl, this._db, this._subtree) {
    _finalizer.attach(this, _vl!.cast(), detach: this);
  }

  // ── construction ──

  /// Open a vector layer on a DEDICATED database at `dbLocation`.
  factory VectorLayer.openSeparate(
    String dbLocation,
    String indexName,
    VectorFormat fmt,
    VectorRuntime rt,
  ) {
    if (indexName.isEmpty) {
      throw VectorLayerException('indexName must be non-empty');
    }
    final cfg = _newConfig(fmt, rt);
    try {
      final ptr = WaveDBNative.vectorLayerOpenSeparate(
          dbLocation, indexName, cfg);
      return VectorLayer._(ptr, null, null);
    } finally {
      calloc.free(cfg);
    }
  }

  /// Open a vector layer sharing `db`'s underlying database. If `subtree`
  /// is supplied, all keys land under that subtree prefix.
  factory VectorLayer.open(
    String indexName,
    WaveDB db,
    VectorFormat fmt,
    VectorRuntime rt, {
    Subtree? subtree,
  }) {
    if (indexName.isEmpty) {
      throw VectorLayerException('indexName must be non-empty');
    }
    final cfg = _newConfig(fmt, rt);
    try {
      final ptr = WaveDBNative.vectorLayerCreate(
        indexName,
        db.databasePtr,
        subtree?.nativePtr ?? nullptr,
        cfg,
      );
      return VectorLayer._(ptr, db, subtree);
    } finally {
      calloc.free(cfg);
    }
  }

  // ── sync API ──

  /// Insert `vec` under `id`, with optional `metadata` bytes. Returns the C
  /// rc (0 on success, <0 on failure).
  int insertSync(String id, List<double> vec, {List<int>? metadata}) {
    _checkOpen();
    final meta = metadata ?? const <int>[];
    final rc = WaveDBNative.vectorLayerInsertSync(_vl!, id, vec, meta);
    if (rc < 0) {
      throw WaveDBException.ioError(
          'vector_layer_insert_sync', 'Insert failed (rc=$rc)');
    }
    return rc;
  }

  /// Search the index for the `k` nearest neighbors of `query`. Returns a
  /// list of [VectorResult] (sorted by the C layer).
  List<VectorResult> searchSync(List<double> query, int k) {
    _checkOpen();
    final resultsPtr = calloc<Pointer<vl_result_t>>();
    final nPtr = calloc<Int32>();
    try {
      final rc = WaveDBNative.vectorLayerSearchSync(
          _vl!, query, k, resultsPtr, nPtr);
      if (rc < 0) {
        throw WaveDBException.ioError(
            'vector_layer_search_sync', 'Search failed (rc=$rc)');
      }
      final n = nPtr.value;
      final arr = resultsPtr.value;
      final out = <VectorResult>[];
      if (arr != nullptr && n > 0) {
        for (var i = 0; i < n; i++) {
          final r = (arr + i).ref;
          final idStr = r.id != nullptr
              ? r.id.cast<Utf8>().toDartString()
              : '';
          final metaLen = r.metadataLen;
          Uint8List meta;
          if (r.metadata != nullptr && metaLen > 0) {
            meta = Uint8List.fromList(r.metadata.asTypedList(metaLen));
          } else {
            meta = Uint8List(0);
          }
          out.add(VectorResult(idStr, r.distance, meta));
        }
        WaveDBNative.vectorLayerFreeResults(arr, n);
      }
      return out;
    } finally {
      calloc.free(resultsPtr);
      calloc.free(nPtr);
    }
  }

  /// Delete the vector with id `id`. Returns the C rc (0 on success).
  int deleteSync(String id) {
    _checkOpen();
    final rc = WaveDBNative.vectorLayerDeleteSync(_vl!, id);
    if (rc < 0) {
      throw WaveDBException.ioError(
          'vector_layer_delete_sync', 'Delete failed (rc=$rc)');
    }
    return rc;
  }

  /// train: IVF (re)compute centroids (k-means); SLSH (re)gen projections;
  /// FLAT no-op. Sync-only.
  int train() {
    _checkOpen();
    final rc = WaveDBNative.vectorLayerTrain(_vl!);
    if (rc < 0) {
      throw WaveDBException.ioError(
          'vector_layer_train', 'Train failed (rc=$rc)');
    }
    return rc;
  }

  /// rebuild: rewrite the index's per-vector entries from stored vectors.
  /// Sync-only.
  int rebuild() {
    _checkOpen();
    final rc = WaveDBNative.vectorLayerRebuild(_vl!);
    if (rc < 0) {
      throw WaveDBException.ioError(
          'vector_layer_rebuild', 'Rebuild failed (rc=$rc)');
    }
    return rc;
  }

  /// Number of vectors currently in the index.
  int count() {
    _checkOpen();
    return WaveDBNative.vectorLayerCount(_vl!);
  }

  /// Reconfigure runtime-tunable params. Format tier is immutable after
  /// create — to change index_type/dim/delimiter/ivf_n_clusters/slsh_*,
  /// drop the layer and recreate.
  int reconfigure(VectorRuntime rt) {
    _checkOpen();
    final rtPtr = calloc<vector_layer_runtime_t>();
    try {
      _fillRuntime(rtPtr.ref, rt);
      final rc = WaveDBNative.vectorLayerReconfigure(_vl!, rtPtr);
      if (rc < 0) {
        throw WaveDBException.ioError(
            'vector_layer_reconfigure', 'Reconfigure failed (rc=$rc)');
      }
      return rc;
    } finally {
      calloc.free(rtPtr);
    }
  }

  // ── lifecycle ──

  /// Close the layer and release C resources. Idempotent.
  void dispose() {
    if (_closed || _vl == null) return;
    _closed = true;
    _finalizer.detach(this);
    WaveDBNative.vectorLayerDestroy(_vl!);
    _vl = null;
  }

  /// Alias for [dispose] — mirrors graph_layer.dart's close().
  void close() => dispose();

  void _checkOpen() {
    if (_closed || _vl == null) {
      throw StateError('VectorLayer is closed');
    }
  }
}

// ── C struct fillers ──

Pointer<vector_layer_config_t> _newConfig(VectorFormat fmt, VectorRuntime rt) {
  final cfg = calloc<vector_layer_config_t>();
  _fillFormat(cfg.ref.format, fmt);
  _fillRuntime(cfg.ref.runtime, rt);
  return cfg;
}

void _fillFormat(vector_layer_format_t ref, VectorFormat fmt) {
  // Validate delimiter: must encode to a single byte.
  int delim;
  if (fmt.delimiter.length == 1) {
    final units = fmt.delimiter.codeUnits;
    if (units.length != 1 || units[0] > 255) {
      throw VectorLayerException(
          'delimiter must encode to a single byte, got ${fmt.delimiter}');
    }
    delim = units[0];
  } else if (fmt.delimiter.isEmpty) {
    delim = 0x2f; // '/'
  } else {
    throw VectorLayerException(
        'delimiter must be a single character, got ${fmt.delimiter}');
  }
  ref.indexType = fmt.indexType;
  ref.dim = fmt.dim;
  ref.delimiter = delim;
  ref.distance = fmt.distance;
  ref.ivfNClusters = fmt.ivfNClusters;
  ref.slshLshTables = fmt.slshLshTables;
  ref.slshHashBits = fmt.slshHashBits;
  ref.slshBucketWidth = fmt.slshBucketWidth;
}

void _fillRuntime(vector_layer_runtime_t ref, VectorRuntime rt) {
  ref.topK = rt.topK;
  ref.syncOnly = rt.syncOnly;
  ref.ivfNprobe = rt.ivfNprobe;
  ref.ivfFlatUntil = rt.ivfFlatUntil;
  ref.slshScanRadius = rt.slshScanRadius;
}