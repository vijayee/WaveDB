// lib/src/identifier.dart
import 'dart:ffi';
import 'dart:convert';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

import 'native/types.dart';
import 'native/wavedb_bindings.dart';

/// Convert between Dart values and native identifier_t
class IdentifierConverter {
  /// Convert Dart value to native identifier_t
  ///
  /// Value can be:
  /// - String: UTF-8 encoded bytes
  /// - Uint8List: Raw bytes
  /// - List<int>: Raw bytes
  static Pointer<identifier_t> toNative(dynamic value) {
    if (value is String) {
      return _fromString(value);
    } else if (value is Uint8List) {
      return _fromUint8List(value);
    } else if (value is List<int>) {
      return _fromListInt(value);
    } else {
      throw ArgumentError('Value must be String, Uint8List, or List<int>');
    }
  }

  static Pointer<identifier_t> _fromString(String s) {
    // Use UTF-8 encoding (not codeUnits which is UTF-16)
    final encoded = utf8.encode(s);
    return _fromUint8List(Uint8List.fromList(encoded));
  }

  static Pointer<identifier_t> _fromUint8List(Uint8List bytes) {
    final ptr = calloc<Uint8>(bytes.length);

    try {
      ptr.asTypedList(bytes.length).setAll(0, bytes);
      // WaveDBNative.identifierCreate creates a buffer internally and copies data
      return WaveDBNative.identifierCreate(ptr, bytes.length);
    } finally {
      // Data is copied, so we can free our buffer
      calloc.free(ptr);
    }
  }

  static Pointer<identifier_t> _fromListInt(List<int> bytes) {
    return _fromUint8List(Uint8List.fromList(bytes));
  }

  /// Create identifier from pointer to data
  static Pointer<identifier_t> fromDataPointer(Pointer<Uint8> data, int length) {
    return WaveDBNative.identifierCreate(data, length);
  }

  /// Convert native identifier_t to Dart value
  ///
  /// Returns String if data is printable ASCII, otherwise Uint8List
  /// Returns null if the identifier pointer is nullptr
  static dynamic fromNative(Pointer<identifier_t> id) {
    if (id == nullptr) {
      return null;
    }

    final bytes = readIdentifierBytes(id);

    if (bytes.isEmpty) {
      return '';
    }

    if (isPrintableASCII(bytes)) {
      return String.fromCharCodes(bytes);
    } else {
      return Uint8List.fromList(bytes);
    }
  }

  /// Read bytes from identifier using FFI buffer accessor
  ///
  /// Converts native identifier_t to a buffer and extracts the raw bytes.
  /// The buffer is properly cleaned up after reading.
  ///
  /// Returns empty list if identifier is null or has no data.
  static List<int> readIdentifierBytes(Pointer<identifier_t> id) {
    if (id == nullptr) {
      return [];
    }

    final buffer = WaveDBNative.identifierToBuffer(id);
    if (buffer == nullptr) {
      return [];
    }

    try {
      final dataPtr = buffer.ref.data;
      final size = buffer.ref.size;

      if (dataPtr == nullptr || size == 0) {
        return [];
      }

      // Copy data to Dart list
      return dataPtr.asTypedList(size).toList();
    } finally {
      WaveDBNative.bufferDestroy(buffer);
    }
  }

  /// Check if bytes are printable ASCII
  ///
  /// Printable ASCII is defined as bytes in range 32-126 (space through tilde)
  static bool isPrintableASCII(List<int> bytes) {
    if (bytes.isEmpty) return true;

    return bytes.every((b) => b >= 32 && b < 127);
  }

  /// Convert Dart value to Uint8List without FFI allocation
  static Uint8List toBytes(dynamic value) {
    if (value is String) {
      return Uint8List.fromList(utf8.encode(value));
    } else if (value is Uint8List) {
      return value;
    } else if (value is List<int>) {
      return Uint8List.fromList(value);
    } else {
      throw ArgumentError('Value must be String, Uint8List, or List<int>');
    }
  }

  /// Destroy a native identifier
  static void destroy(Pointer<identifier_t> id) {
    WaveDBNative.identifierDestroy(id);
  }
}
