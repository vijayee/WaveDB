// lib/src/graph_layer.dart
import 'dart:ffi';
import 'dart:io';
import 'package:ffi/ffi.dart';
import 'native/types.dart';
import 'native/wavedb_bindings.dart';

GraphQuery g([GraphLayer? layer]) => GraphQuery(layer ?? _defaultGraph);
GraphLayer? _defaultGraph;

/// Graph layer for WaveDB
///
/// Provides triple insert/delete and DSL query execution on top of
/// WaveDB's hierarchical key-value store.
class GraphLayer implements Finalizable {
  Pointer<graph_layer_t>? _layer;
  bool _closed = false;
  String? _tempDir;

  static final NativeFinalizer _finalizer = NativeFinalizer(
    WaveDBNative.graphLayerDestroyNative.cast(),
  );

  GraphLayer([String? path]) {
    String actualPath;
    if (path != null) {
      actualPath = path;
    } else {
      _tempDir = Directory.systemTemp.createTempSync('wavedb_graph_').path;
      actualPath = _tempDir!;
    }
    _layer = WaveDBNative.graphLayerCreate(actualPath);
    _finalizer.attach(this, _layer!.cast(), detach: this);
    if (_defaultGraph == null) _defaultGraph = this;
  }

  /// Execute a DSL query and return results as a list of strings.
  List<String> exec(Object dsl) {
    _checkOpen();
    final dslStr = dsl is GraphQuery ? dsl._toDSL() : dsl as String;
    final resultPtr = WaveDBNative.graphParseExecute(_layer!, dslStr);
    if (resultPtr == nullptr) return const [];

    try {
      final count = WaveDBNative.graphResultCount(resultPtr);
      if (count == 0) return const [];

      final verts = WaveDBNative.graphResultVertices(resultPtr);
      final results = <String>[];
      for (int i = 0; i < count; i++) {
        final strPtr = verts.elementAt(i).value;
        if (strPtr != nullptr) {
          results.add(strPtr.toDartString());
        }
      }
      return results;
    } finally {
      WaveDBNative.graphResultDestroy(resultPtr);
    }
  }

  /// Execute a DSL query and return the count only.
  int count(Object dsl) {
    _checkOpen();
    final dslStr = dsl is GraphQuery ? dsl._toDSL() : dsl as String;
    return WaveDBNative.graphParseCount(_layer!, dslStr);
  }

  /// Insert a triple synchronously.
  void insertSync(String s, String p, String o) {
    _checkOpen();
    final rc = WaveDBNative.graphInsertSync(_layer!, s, p, o);
    if (rc != 0) {
      throw GraphLayerException('Insert failed ($rc)');
    }
  }

  /// Delete a triple synchronously.
  void deleteSync(String s, String p, String o) {
    _checkOpen();
    final rc = WaveDBNative.graphDeleteSync(_layer!, s, p, o);
    if (rc != 0) {
      throw GraphLayerException('Delete failed ($rc)');
    }
  }

  /// Close the graph layer and release C resources.
  void close() {
    if (!_closed && _layer != null) {
      if (_defaultGraph == this) _defaultGraph = null;
      _finalizer.detach(this);
      WaveDBNative.graphLayerDestroy(_layer!);
      _layer = null;
      _closed = true;
    }
    // Clean up temp directory if we created one
    if (_tempDir != null) {
      try {
        Directory(_tempDir!).deleteSync(recursive: true);
      } catch (_) {
        // Ignore cleanup errors
      }
      _tempDir = null;
    }
  }

  void _checkOpen() {
    if (_closed || _layer == null) {
      throw StateError('GraphLayer is closed');
    }
  }
}

/// Exception thrown by graph layer operations
class GraphLayerException implements Exception {
  final String message;
  const GraphLayerException(this.message);

  @override
  String toString() => 'GraphLayerException: $message';
}

/// Query builder for graph traversals
class GraphQuery {
  GraphLayer? _layer;
  final List<String> _steps = [];

  GraphQuery([GraphLayer? layer]) : _layer = layer;

  GraphQuery V(String id) { _steps.add('V("$id")'); return this; }
  GraphQuery Out(String pred) { _steps.add('Out("$pred")'); return this; }
  GraphQuery In(String pred) { _steps.add('In("$pred")'); return this; }
  GraphQuery Has(String pred, String val) { _steps.add('Has("$pred","$val")'); return this; }
  GraphQuery And(GraphQuery sub) { _steps.add('And(${sub._toDSL()})'); return this; }
  GraphQuery Or(GraphQuery sub) { _steps.add('Or(${sub._toDSL()})'); return this; }
  GraphQuery Limit(int n) { _steps.add('Limit($n)'); return this; }

  String _toDSL() => 'g.${_steps.join('.')}';

  /// Execute the query and return results as a list of strings.
  List<String> All() {
    if (_layer == null) throw StateError('Query not bound to a GraphLayer');
    return _layer!.exec(this);
  }

  @override
  String toString() => _toDSL();
}
