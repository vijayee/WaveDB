// test/subtree_test.dart
import 'dart:io';
import 'dart:typed_data';
import 'package:test/test.dart';
import 'package:wavedb/wavedb.dart';

/// Test fixture for managing database lifecycle
class SubtreeTestFixture {
  late String dbPath;
  WaveDB? db;

  void setUp() {
    final tempDir = Directory.systemTemp.createTempSync('wavedb_subtree_test_');
    dbPath = tempDir.path;
    db = WaveDB(dbPath);
  }

  void tearDown() {
    db?.close();
    db = null;
    try {
      Directory(dbPath).deleteSync(recursive: true);
    } catch (_) {}
  }
}

void main() {
  group('Subtree', () {
    late SubtreeTestFixture fixture;

    setUp(() {
      fixture = SubtreeTestFixture();
      fixture.setUp();
    });

    tearDown(() {
      fixture.tearDown();
    });

    group('open/close', () {
      test('should open and close a subtree', () {
        final subtree = fixture.db!.openSubtree('layer/graph');
        expect(subtree.isClosed, isFalse);
        expect(subtree.delimiter, equals('/'));
        subtree.close();
        expect(subtree.isClosed, isTrue);
      });

      test('should support custom delimiter', () {
        final subtree = fixture.db!.openSubtree('layer', delimiter: '.');
        expect(subtree.delimiter, equals('.'));
        subtree.close();
      });

      test('should handle double close', () {
        final subtree = fixture.db!.openSubtree('test');
        subtree.close();
        subtree.close(); // Should not throw
        expect(subtree.isClosed, isTrue);
      });
    });

    group('sync put/get/del', () {
      test('should put and get a string value', () {
        final subtree = fixture.db!.openSubtree('app');
        try {
          subtree.putSync('users/alice/name', 'Alice');
          final value = subtree.getSync('users/alice/name');
          expect(value, equals('Alice'));
        } finally {
          subtree.close();
        }
      });

      test('should put and get binary value', () {
        final subtree = fixture.db!.openSubtree('app');
        try {
          final binary = Uint8List.fromList([0x01, 0x02, 0x03, 0xFF]);
          subtree.putSync('binary/key', binary);
          final value = subtree.getSync('binary/key');
          expect(value, isA<Uint8List>());
          expect(value, equals(binary));
        } finally {
          subtree.close();
        }
      });

      test('should return null for missing keys', () {
        final subtree = fixture.db!.openSubtree('app');
        try {
          final value = subtree.getSync('missing/key');
          expect(value, isNull);
        } finally {
          subtree.close();
        }
      });

      test('should delete a key', () {
        final subtree = fixture.db!.openSubtree('app');
        try {
          subtree.putSync('key', 'value');
          subtree.delSync('key');
          final value = subtree.getSync('key');
          expect(value, isNull);
        } finally {
          subtree.close();
        }
      });

      test('should handle missing key delete', () {
        final subtree = fixture.db!.openSubtree('app');
        try {
          // Deleting non-existent key should not throw
          expect(() => subtree.delSync('missing/key'), returnsNormally);
        } finally {
          subtree.close();
        }
      });
    });

    group('cross-subtree isolation', () {
      test('subtrees with different prefixes should be isolated', () {
        final subtree1 = fixture.db!.openSubtree('namespace1');
        final subtree2 = fixture.db!.openSubtree('namespace2');
        try {
          subtree1.putSync('key', 'value1');
          subtree2.putSync('key', 'value2');

          expect(subtree1.getSync('key'), equals('value1'));
          expect(subtree2.getSync('key'), equals('value2'));
        } finally {
          subtree1.close();
          subtree2.close();
        }
      });

      test('subtree keys should not leak to main database', () {
        final subtree = fixture.db!.openSubtree('isolated');
        try {
          subtree.putSync('testkey', 'testvalue');

          // The key in the main db is prefixed: "isolated/testkey"
          // Direct access with the same bare key should not find it
          expect(fixture.db!.getSync('testkey'), isNull);
        } finally {
          subtree.close();
        }
      });
    });

    group('count', () {
      test('should count entries under the subtree prefix', () {
        final subtree = fixture.db!.openSubtree('metrics');
        try {
          expect(subtree.count(), equals(0));

          subtree.putSync('counter1', '10');
          subtree.putSync('counter2', '20');
          subtree.putSync('counter3', '30');

          expect(subtree.count(), equals(3));
        } finally {
          subtree.close();
        }
      });

      test('count should decrease after delete', () {
        final subtree = fixture.db!.openSubtree('metrics');
        try {
          subtree.putSync('a', '1');
          subtree.putSync('b', '2');
          expect(subtree.count(), equals(2));

          subtree.delSync('a');
          expect(subtree.count(), equals(1));
        } finally {
          subtree.close();
        }
      });
    });

    group('deleteSubtree (prefix delete)', () {
      test('should delete all keys under a prefix', () {
        // Put keys in the database under a known prefix
        fixture.db!.putSync('myapp/users/alice', 'Alice');
        fixture.db!.putSync('myapp/users/bob', 'Bob');
        fixture.db!.putSync('myapp/settings/theme', 'dark');

        fixture.db!.deleteSubtree('myapp');

        expect(fixture.db!.getSync('myapp/users/alice'), isNull);
        expect(fixture.db!.getSync('myapp/users/bob'), isNull);
        expect(fixture.db!.getSync('myapp/settings/theme'), isNull);
      });
    });

    group('batch', () {
      test('should execute batch operations on subtree', () {
        final subtree = fixture.db!.openSubtree('batchns');
        try {
          subtree.batchSync([
            {'type': 'put', 'key': 'key1', 'value': 'value1'},
            {'type': 'put', 'key': 'key2', 'value': 'value2'},
            {'type': 'del', 'key': 'nonexistent'},
          ]);

          expect(subtree.getSync('key1'), equals('value1'));
          expect(subtree.getSync('key2'), equals('value2'));
        } finally {
          subtree.close();
        }
      });
    });

    group('scan', () {
      test('should scan entries under a prefix within subtree', () {
        final subtree = fixture.db!.openSubtree('scan');
        try {
          subtree.putSync('users/alice', 'Alice');
          subtree.putSync('users/bob', 'Bob');
          subtree.putSync('items/x', 'Item X');

          final results = subtree.scanSyncRaw('users');
          expect(results.length, equals(2));

          final keys = results.map((e) => e['key'] as String).toList();
          // Keys returned by subtree scan have the subtree prefix stripped
          expect(keys, contains('users/alice'));
          expect(keys, contains('users/bob'));
        } finally {
          subtree.close();
        }
      });

      test('should return empty list for no matches', () {
        final subtree = fixture.db!.openSubtree('scan');
        try {
          final results = subtree.scanSyncRaw('nonexistent');
          expect(results, isEmpty);
        } finally {
          subtree.close();
        }
      });
    });

    group('snapshot', () {
      test('should snapshot the underlying database', () {
        final subtree = fixture.db!.openSubtree('snap');
        try {
          subtree.putSync('key', 'value');
          // Snapshot should not throw
          expect(() => subtree.snapshot(), returnsNormally);
        } finally {
          subtree.close();
        }
      });
    });

    group('operations on closed subtree', () {
      test('should throw on put after close', () {
        final subtree = fixture.db!.openSubtree('closed');
        subtree.close();
        expect(
          () => subtree.putSync('key', 'value'),
          throwsA(isA<WaveDBException>()),
        );
      });

      test('should throw on get after close', () {
        final subtree = fixture.db!.openSubtree('closed');
        subtree.close();
        expect(
          () => subtree.getSync('key'),
          throwsA(isA<WaveDBException>()),
        );
      });

      test('should throw on del after close', () {
        final subtree = fixture.db!.openSubtree('closed');
        subtree.close();
        expect(
          () => subtree.delSync('key'),
          throwsA(isA<WaveDBException>()),
        );
      });
    });

    group('schema collision', () {
      test('GraphLayer without subtree should fail on GraphQLLayer database', () {
        // Create a GraphQL layer on a temp database
        final tempDir = Directory.systemTemp.createTempSync('wavedb_collision_test_');
        final tempPath = tempDir.path;
        try {
          final graphql = GraphQLLayer();
          graphql.create(GraphQLLayerConfig(path: tempPath));
          graphql.parseSchema('type User { name: String }');
          graphql.close();

          // Now try to create a GraphLayer on the same database without subtree
          // This should fail with a conflict error (error code -3)
          expect(
            () => GraphLayer(tempPath),
            throwsA(isA<WaveDBException>().having((e) => e.code, 'code', 'CONFLICT')),
          );
        } finally {
          try {
            Directory(tempPath).deleteSync(recursive: true);
          } catch (_) {}
        }
      });
    });

    group('async operations', () {
      test('should put and get a value asynchronously', () async {
        final subtree = fixture.db!.openSubtree('async_test');
        await subtree.put('key1', 'value1');
        final result = await subtree.get('key1');
        expect(result, equals('value1'));
        subtree.close();
      });

      test('should return null for missing key asynchronously', () async {
        final subtree = fixture.db!.openSubtree('async_test');
        final result = await subtree.get('nonexistent');
        expect(result, isNull);
        subtree.close();
      });

      test('should delete a value asynchronously', () async {
        final subtree = fixture.db!.openSubtree('async_test');
        await subtree.put('key1', 'value1');
        final result = await subtree.get('key1');
        expect(result, equals('value1'));

        await subtree.del('key1');
        final resultAfterDelete = await subtree.get('key1');
        expect(resultAfterDelete, isNull);
        subtree.close();
      });

      test('should handle multiple async operations concurrently', () async {
        final subtree = fixture.db!.openSubtree('async_concurrent');

        // Fire multiple puts concurrently
        await Future.wait([
          subtree.put('key1', 'value1'),
          subtree.put('key2', 'value2'),
          subtree.put('key3', 'value3'),
        ]);

        // Verify all values
        expect(await subtree.get('key1'), equals('value1'));
        expect(await subtree.get('key2'), equals('value2'));
        expect(await subtree.get('key3'), equals('value3'));
        subtree.close();
      });

      test('should reject operations on closed subtree', () async {
        final subtree = fixture.db!.openSubtree('async_closed');
        await subtree.put('key1', 'value1');
        subtree.close();

        expect(
          () => subtree.put('key2', 'value2'),
          throwsA(isA<WaveDBException>()),
        );
      });
    });
  });
}