// lib/src/iterator.dart
import 'dart:async';
import 'dart:ffi';
import 'package:ffi/ffi.dart';
import 'native/types.dart';
import 'native/wavedb_bindings.dart';
import 'path.dart';
import 'identifier.dart';
import 'exceptions.dart';

/// Stream-based iterator for WaveDB entries
class WaveDBIterator {
  final Pointer<database_t> _db;
  final String _delimiter;
  final dynamic _startPath;
  final dynamic _endPath;
  /// Reserved for future reverse iteration support
  /// (C API does not yet support reverse scanning)
  final bool _reverse;
  final bool _keys;
  final bool _values;

  Pointer<database_iterator_t>? _nativeIterator;
  bool _started = false;
  bool _done = false;

  WaveDBIterator({
    required Pointer<database_t> db,
    required String delimiter,
    dynamic startPath,
    dynamic endPath,
    bool reverse = false,
    bool keys = true,
    bool values = true,
  })  : _db = db,
        _delimiter = delimiter,
        _startPath = startPath,
        _endPath = endPath,
        _reverse = reverse,
        _keys = keys,
        _values = values;

  /// Get the stream of key-value pairs
  Stream<KeyValue> get stream => _createStream();

  Stream<KeyValue> _createStream() async* {
    try {
      _startIterator();

      while (!_done) {
        final entry = _readNext();
        if (entry == null) {
          _done = true;
          break;
        }

        yield KeyValue(
          key: _keys ? entry.key : null,
          value: _values ? entry.value : null,
        );
      }
    } finally {
      _endIterator();
    }
  }

  void _startIterator() {
    if (_started) return;
    _started = true;

    Pointer<path_t>? startNative;
    Pointer<path_t>? endNative;

    try {
      // Convert start/end paths
      if (_startPath != null) {
        startNative = PathConverter.toNative(_startPath, _delimiter);
      }
      if (_endPath != null) {
        endNative = PathConverter.toNative(_endPath, _delimiter);
      }

      // Create native iterator
      // NOTE: database_scan_start function does not exist in C API yet
      // This will throw NOT_SUPPORTED or symbol not found error when used
      // TODO: Implement database_scan_* in C API for full iterator support
      _nativeIterator = WaveDBNative.databaseScanStart(
        _db,
        startNative ?? nullptr,
        endNative ?? nullptr,
      );

      if (_nativeIterator == nullptr) {
        throw WaveDBException.ioError('scan_start', 'Failed to create iterator');
      }

      // If we reach here, the scan API exists and takes ownership of paths
      // (The C implementation would need to take ownership to avoid leaks)
    } catch (e) {
      // Clean up paths if scan_start failed or doesn't exist
      if (startNative != null) {
        WaveDBNative.pathDestroy(startNative);
      }
      if (endNative != null) {
        WaveDBNative.pathDestroy(endNative);
      }
      rethrow;
    }
  }

  KeyValue? _readNext() {
    if (_nativeIterator == null || _nativeIterator == nullptr) {
      return null;
    }

    final pathPtr = calloc<Pointer<path_t>>();
    final valuePtr = calloc<Pointer<identifier_t>>();

    try {
      final rc = WaveDBNative.databaseScanNext(
        _nativeIterator!,
        pathPtr,
        valuePtr,
      );

      if (rc != 0) {
        _done = true;
        return null;
      }

      final path = pathPtr.value;
      final value = valuePtr.value;

      try {
        // Note: PathConverter.fromNative and IdentifierConverter.fromNative currently
        // return empty data due to missing C accessor functions.
        // This will be fixed when C API adds identifier_get_data and path_get_identifiers.
        final keyResult = PathConverter.fromNative(path, _delimiter);
        final valueResult = IdentifierConverter.fromNative(value);

        return KeyValue(key: keyResult, value: valueResult);
      } finally {
        WaveDBNative.pathDestroy(path);
        WaveDBNative.identifierDestroy(value);
      }
    } finally {
      calloc.free(pathPtr);
      calloc.free(valuePtr);
    }
  }

  void _endIterator() {
    if (_nativeIterator != null && _nativeIterator != nullptr) {
      WaveDBNative.databaseScanEnd(_nativeIterator!);
      _nativeIterator = null;
    }
  }
}

/// Key-value pair for iterator results
class KeyValue {
  final dynamic key;
  final dynamic value;

  KeyValue({this.key, this.value});

  @override
  String toString() => 'KeyValue(key: $key, value: $value)';

  @override
  bool operator ==(Object other) {
    if (identical(this, other)) return true;
    return other is KeyValue && other.key == key && other.value == value;
  }

  @override
  int get hashCode => Object.hash(key, value);
}
