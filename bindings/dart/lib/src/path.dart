// lib/src/path.dart
import 'dart:ffi';
import 'dart:convert';

import 'package:ffi/ffi.dart';

import 'native/types.dart';
import 'native/wavedb_bindings.dart';
import 'exceptions.dart';
import 'identifier.dart';

/// Convert between Dart keys and native path_t
class PathConverter {
  /// Convert Dart key to native path_t
  ///
  /// Key can be:
  /// - String: Split by delimiter
  /// - List<String>: Direct path components
  static Pointer<path_t> toNative(dynamic key, String delimiter) {
    final path = WaveDBNative.pathCreate();

    try {
      List<String> parts;

      if (key is String) {
        // Split string by delimiter
        parts = key.split(delimiter).where((s) => s.isNotEmpty).toList();
      } else if (key is List) {
        // Already a list of path components
        parts = key.map((e) => e.toString()).toList();
      } else {
        throw ArgumentError('Key must be String or List<String>');
      }

      if (parts.isEmpty) {
        throw ArgumentError('Key cannot be empty');
      }

      // Create identifier for each path component and append
      for (final part in parts) {
        final id = _createIdentifierFromString(part);
        try {
          final rc = WaveDBNative.pathAppend(path, id);
          if (rc != 0) {
            throw WaveDBException.invalidPath(part, 'Failed to append to path');
          }
        } finally {
          // path_append increments refcount, does NOT take ownership
          // Must always destroy our reference after append
          WaveDBNative.identifierDestroy(id);
        }
      }

      return path;
    } catch (e) {
      // Clean up on error
      WaveDBNative.pathDestroy(path);
      rethrow;
    }
  }

  /// Create identifier from a string
  static Pointer<identifier_t> _createIdentifierFromString(String s) {
    // Use UTF-8 encoding, not UTF-16 code units
    final bytes = utf8.encode(s);
    final ptr = calloc<Uint8>(bytes.length);

    try {
      for (var i = 0; i < bytes.length; i++) {
        ptr[i] = bytes[i];
      }
      return WaveDBNative.identifierCreate(ptr, bytes.length);
    } finally {
      // identifierCreate copies the data, so we can free the buffer
      calloc.free(ptr);
    }
  }

  /// Convert native path_t to Dart key
  ///
  /// Returns String (joined by delimiter) or List<String> (if asArray=true)
  /// Uses FFI path_length and path_get functions to traverse the native path.
  static dynamic fromNative(Pointer<path_t> path, String delimiter, {bool asArray = false}) {
    if (path == nullptr) {
      return asArray ? <String>[] : '';
    }

    final length = WaveDBNative.pathLength(path);
    if (length == 0) {
      return asArray ? <String>[] : '';
    }

    final parts = <String>[];

    for (var i = 0; i < length; i++) {
      final idPtr = WaveDBNative.pathGet(path, i);
      if (idPtr == nullptr) continue;

      final bytes = IdentifierConverter.readIdentifierBytes(idPtr);
      if (IdentifierConverter.isPrintableASCII(bytes)) {
        parts.add(String.fromCharCodes(bytes));
      } else {
        // For non-ASCII, decode as UTF-8 with error handling
        parts.add(utf8.decode(bytes, allowMalformed: true));
      }
    }

    if (asArray) {
      return parts;
    } else {
      return parts.join(delimiter);
    }
  }

  /// Destroy a native path
  static void destroy(Pointer<path_t> path) {
    WaveDBNative.pathDestroy(path);
  }
}
