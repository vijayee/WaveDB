// lib/src/object_ops.dart
import 'dart:typed_data';

/// Helper class for flattening objects and reconstructing them
class ObjectOps {
  /// Flatten a nested object into batch operations (iterative)
  ///
  /// Returns a list of operations: {'type': 'put', 'key': List<String>, 'value': dynamic}
  static List<Map<String, dynamic>> flattenObject(
    dynamic key,
    Map<String, dynamic> obj,
    String delimiter,
  ) {
    final operations = <Map<String, dynamic>>[];
    final basePath = <String>[];

    // Add key prefix if provided
    if (key != null) {
      if (key is String) {
        basePath.addAll(key.split(delimiter).where((s) => s.isNotEmpty));
      } else if (key is List) {
        basePath.addAll(key.map((e) => e.toString()));
      }
    }

    // Use explicit stack to avoid recursion depth issues
    // Stack entries: (value, current path)
    final stack = <MapEntry<dynamic, List<String>>>[
      MapEntry(obj, List<String>.from(basePath)),
    ];

    while (stack.isNotEmpty) {
      final entry = stack.removeLast();
      final value = entry.key;
      final pathParts = entry.value;

      if (value is Map<String, dynamic> || value is Map) {
        // Object - push children to stack in reverse order
        final map = value as Map;
        final keys = map.keys.toList().reversed;

        for (final k in keys) {
          final newPath = List<String>.from(pathParts);
          newPath.add(k.toString());
          stack.add(MapEntry(map[k], newPath));
        }
      } else if (value is Uint8List) {
        // Binary data (Uint8List) - treat as leaf value, not as array to flatten
        operations.add({
          'type': 'put',
          'key': pathParts,
          'value': value,
        });
      } else if (value is List) {
        // Array - push elements in reverse order
        for (var i = value.length - 1; i >= 0; i--) {
          final newPath = List<String>.from(pathParts);
          newPath.add(i.toString());
          stack.add(MapEntry(value[i], newPath));
        }
      } else {
        // Leaf value - create put operation
        operations.add({
          'type': 'put',
          'key': pathParts,
          'value': _convertLeafValue(value),
        });
      }
    }

    return operations;
  }

  /// Convert leaf value to storable format
  static dynamic _convertLeafValue(dynamic value) {
    if (value == null) return '';
    if (value is String || value is Uint8List || value is List<int>) return value;
    return value.toString();
  }

  /// Reconstruct a nested object from path-value pairs (iterative)
  static Map<String, dynamic> reconstructObject(
    List<MapEntry<List<String>, dynamic>> entries,
    List<String> basePath,
  ) {
    final result = <String, dynamic>{};

    for (final entry in entries) {
      final relativePath = _getRelativePath(entry.key, basePath);
      if (relativePath.isEmpty) continue;

      // Build path to parent, then set value
      var current = result;

      for (var i = 0; i < relativePath.length - 1; i++) {
        final k = relativePath[i];
        if (!current.containsKey(k)) {
          current[k] = <String, dynamic>{};
        }
        current = current[k] as Map<String, dynamic>;
      }

      current[relativePath.last] = entry.value;
    }

    // Convert numeric-keyed objects to arrays
    return _convertArrays(result) as Map<String, dynamic>;
  }

  /// Get path relative to base path
  static List<String> _getRelativePath(List<String> path, List<String> basePath) {
    if (basePath.isEmpty) return path;
    if (path.length < basePath.length) return [];

    for (var i = 0; i < basePath.length; i++) {
      if (path[i] != basePath[i]) return [];
    }

    return path.sublist(basePath.length);
  }

  /// Convert objects with all-numeric keys to arrays (iterative)
  static dynamic _convertArrays(dynamic obj) {
    if (obj is! Map<String, dynamic>) return obj;

    final keys = obj.keys.toList();

    // Check if all keys are numeric
    if (_isContiguousNumericKeys(keys)) {
      final indices = keys.map(int.parse).toList()..sort();
      final arr = <dynamic>[];

      for (final idx in indices) {
        arr.add(_convertArrays(obj[idx.toString()]));
      }
      return arr;
    }

    // Recursively convert nested objects
    final result = <String, dynamic>{};
    obj.forEach((key, value) {
      result[key] = _convertArrays(value);
    });

    return result;
  }

  /// Check if keys are contiguous numeric indices starting at 0
  static bool _isContiguousNumericKeys(List<String> keys) {
    if (keys.isEmpty) return false;

    final indices = <int>[];
    for (final key in keys) {
      final num = int.tryParse(key);
      if (num == null) return false;
      indices.add(num);
    }

    indices.sort();
    return indices.first == 0 && indices.last == indices.length - 1;
  }
}
