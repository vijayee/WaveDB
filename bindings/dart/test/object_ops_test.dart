// test/object_ops_test.dart
import 'dart:typed_data';
import 'package:test/test.dart';
import 'package:wavedb/src/object_ops.dart';

void main() {
  group('ObjectOps.flattenObject', () {
    test('should flatten simple object', () {
      final ops = ObjectOps.flattenObject(null, {
        'name': 'Alice',
        'age': '30',
      }, '/');

      expect(ops.length, equals(2));
      expect(ops.any((op) =>
        op['type'] == 'put' &&
        (op['key'] as List).join('/') == 'name' &&
        op['value'] == 'Alice'
      ), isTrue);
      expect(ops.any((op) =>
        op['type'] == 'put' &&
        (op['key'] as List).join('/') == 'age' &&
        op['value'] == '30'
      ), isTrue);
    });

    test('should flatten nested object', () {
      final ops = ObjectOps.flattenObject(null, {
        'users': {
          'alice': {'name': 'Alice'},
        },
      }, '/');

      expect(ops.length, equals(1));
      expect((ops[0]['key'] as List).join('/'), equals('users/alice/name'));
      expect(ops[0]['value'], equals('Alice'));
    });

    test('should flatten arrays with numeric indices', () {
      final ops = ObjectOps.flattenObject(null, {
        'items': ['a', 'b', 'c'],
      }, '/');

      expect(ops.length, equals(3));
      expect((ops[0]['key'] as List).join('/'), equals('items/0'));
      expect((ops[1]['key'] as List).join('/'), equals('items/1'));
      expect((ops[2]['key'] as List).join('/'), equals('items/2'));
    });

    test('should handle key prefix', () {
      final ops = ObjectOps.flattenObject('prefix', {
        'name': 'Alice',
      }, '/');

      expect(ops.length, equals(1));
      expect((ops[0]['key'] as List).join('/'), equals('prefix/name'));
    });

    test('should handle key prefix as list', () {
      final ops = ObjectOps.flattenObject(['users', 'alice'], {
        'name': 'Alice',
      }, '/');

      expect(ops.length, equals(1));
      expect((ops[0]['key'] as List).join('/'), equals('users/alice/name'));
    });

    test('should handle deeply nested object (iterative - no stack overflow)', () {
      // Create a deeply nested object that would overflow recursive stack
      dynamic obj = <String, dynamic>{'value': 'deep'};
      for (var i = 0; i < 10000; i++) {
        obj = <String, dynamic>{'level$i': obj};
      }

      // Should not throw stack overflow
      expect(() => ObjectOps.flattenObject(null, obj as Map<String, dynamic>, '/'), returnsNormally);
    });

    test('should convert non-string values to string', () {
      final ops = ObjectOps.flattenObject(null, {
        'count': 42,
        'flag': true,
      }, '/');

      expect(ops.length, equals(2));
      expect(ops.any((op) => op['value'] == '42'), isTrue);
      expect(ops.any((op) => op['value'] == 'true'), isTrue);
    });

    test('should handle null value', () {
      final ops = ObjectOps.flattenObject(null, {
        'empty': null,
      }, '/');

      expect(ops.length, equals(1));
      expect(ops[0]['value'], equals(''));
    });

    test('should preserve binary values (Uint8List)', () {
      final ops = ObjectOps.flattenObject(null, {
        'binary': Uint8List.fromList([1, 2, 3]),
      }, '/');

      expect(ops.length, equals(1));
      expect(ops[0]['value'], isA<Uint8List>());
      expect((ops[0]['value'] as Uint8List), equals([1, 2, 3]));
    });

    test('should handle empty object', () {
      final ops = ObjectOps.flattenObject(null, {}, '/');
      expect(ops, isEmpty);
    });
  });

  group('ObjectOps.reconstructObject', () {
    test('should reconstruct simple object', () {
      final entries = [
        MapEntry(['name'], 'Alice'),
        MapEntry(['age'], '30'),
      ];

      final obj = ObjectOps.reconstructObject(entries, []);

      expect(obj['name'], equals('Alice'));
      expect(obj['age'], equals('30'));
    });

    test('should reconstruct nested object', () {
      final entries = [
        MapEntry(['users', 'alice', 'name'], 'Alice'),
        MapEntry(['users', 'alice', 'age'], '30'),
      ];

      final obj = ObjectOps.reconstructObject(entries, []);

      expect(obj['users']['alice']['name'], equals('Alice'));
      expect(obj['users']['alice']['age'], equals('30'));
    });

    test('should convert contiguous numeric keys to arrays', () {
      final entries = [
        MapEntry(['items', '0'], 'a'),
        MapEntry(['items', '1'], 'b'),
        MapEntry(['items', '2'], 'c'),
      ];

      final obj = ObjectOps.reconstructObject(entries, []);

      expect(obj['items'], isA<List>());
      expect(obj['items'], equals(['a', 'b', 'c']));
    });

    test('should not convert non-contiguous keys to arrays', () {
      final entries = [
        MapEntry(['data', 'name'], 'test'),
        MapEntry(['data', 'value'], '123'),
      ];

      final obj = ObjectOps.reconstructObject(entries, []);

      // Should remain as object, not array
      expect(obj['data'], isA<Map>());
    });

    test('should respect base path', () {
      final entries = [
        MapEntry(['users', 'alice', 'name'], 'Alice'),
        MapEntry(['users', 'bob', 'name'], 'Bob'),
      ];

      final obj = ObjectOps.reconstructObject(entries, ['users']);

      expect(obj['alice']['name'], equals('Alice'));
      expect(obj['bob']['name'], equals('Bob'));
    });

    test('should return empty object for non-matching base path', () {
      final entries = [
        MapEntry(['other', 'path'], 'value'),
      ];

      final obj = ObjectOps.reconstructObject(entries, ['users']);

      expect(obj, isEmpty);
    });
  });

  group('ObjectOps array conversion', () {
    test('should convert contiguous numeric keys starting at 0 to array', () {
      final entries = [
        MapEntry(['arr', '0'], 'first'),
        MapEntry(['arr', '1'], 'second'),
        MapEntry(['arr', '2'], 'third'),
      ];

      final obj = ObjectOps.reconstructObject(entries, []);
      expect(obj['arr'], equals(['first', 'second', 'third']));
    });

    test('should not convert non-contiguous numeric keys to array', () {
      final entries = [
        MapEntry(['arr', '0'], 'first'),
        MapEntry(['arr', '2'], 'third'), // missing index 1
      ];

      final obj = ObjectOps.reconstructObject(entries, []);
      // Should be a Map, not a List
      expect(obj['arr'], isA<Map>());
    });

    test('should not convert numeric keys not starting at 0 to array', () {
      final entries = [
        MapEntry(['arr', '1'], 'second'),
        MapEntry(['arr', '2'], 'third'),
      ];

      final obj = ObjectOps.reconstructObject(entries, []);
      // Should be a Map, not a List
      expect(obj['arr'], isA<Map>());
    });

    test('should not convert mixed keys to array', () {
      final entries = [
        MapEntry(['arr', '0'], 'first'),
        MapEntry(['arr', 'name'], 'value'),
      ];

      final obj = ObjectOps.reconstructObject(entries, []);
      // Should be a Map, not a List
      expect(obj['arr'], isA<Map>());
    });

    test('should handle empty object reconstruction', () {
      final obj = ObjectOps.reconstructObject([], []);
      expect(obj, isEmpty);
    });

    test('should convert nested arrays correctly', () {
      final entries = [
        MapEntry(['matrix', '0', '0'], 'a'),
        MapEntry(['matrix', '0', '1'], 'b'),
        MapEntry(['matrix', '1', '0'], 'c'),
        MapEntry(['matrix', '1', '1'], 'd'),
      ];

      final obj = ObjectOps.reconstructObject(entries, []);
      expect(obj['matrix'], isA<List>());
      expect(obj['matrix'][0], equals(['a', 'b']));
      expect(obj['matrix'][1], equals(['c', 'd']));
    });
  });
}