// test/wavedb_test.dart
import 'dart:io';
import 'dart:typed_data';
import 'package:test/test.dart';
import 'package:wavedb/wavedb.dart';

/// Test fixture for managing database lifecycle
class TestFixture {
  late String dbPath;
  WaveDB? db;

  Future<void> setUp() async {
    // Create temp directory for test database
    final tempDir = await Directory.systemTemp.createTemp('wavedb_test_');
    dbPath = tempDir.path;
    db = WaveDB(dbPath);
  }

  Future<void> tearDown() async {
    // Close database
    db?.close();
    db = null;

    // Clean up temp directory
    try {
      await Directory(dbPath).delete(recursive: true);
    } catch (_) {
      // Ignore cleanup errors
    }
  }
}

void main() {
  group('WaveDB Integration Tests', () {
    late TestFixture fixture;

    setUp(() async {
      fixture = TestFixture();
      await fixture.setUp();
    });

    tearDown(() async {
      await fixture.tearDown();
    });

    group('put/get sync', () {
      test('should put and get a string value', () {
        fixture.db!.putSync('users/alice/name', 'Alice');
        final value = fixture.db!.getSync('users/alice/name');
        expect(value, equals('Alice'));
      });

      test('should put and get binary value', () {
        final binary = Uint8List.fromList([0x01, 0x02, 0x03, 0xFF]);
        fixture.db!.putSync('binary/key', binary);
        final value = fixture.db!.getSync('binary/key');
        expect(value, isA<Uint8List>());
        expect(value, equals(binary));
      });

      test('should handle string and array keys', () {
        fixture.db!.putSync('users/alice/name', 'Alice');
        fixture.db!.putSync(['users', 'bob', 'name'], 'Bob');

        expect(fixture.db!.getSync('users/alice/name'), equals('Alice'));
        expect(fixture.db!.getSync(['users', 'bob', 'name']), equals('Bob'));
      });

      test('should return null for missing keys', () {
        final value = fixture.db!.getSync('missing/key');
        expect(value, isNull);
      });

      test('should throw on closed database', () {
        fixture.db!.close();
        expect(
          () => fixture.db!.putSync('key', 'value'),
          throwsA(isA<WaveDBException>().having((e) => e.code, 'code', 'DATABASE_CLOSED')),
        );
      });

      test('should throw on null value', () {
        expect(
          () => fixture.db!.putSync('key', null),
          throwsArgumentError,
        );
      });
    });

    group('delete sync', () {
      test('should delete a key', () {
        fixture.db!.putSync('key', 'value');
        fixture.db!.delSync('key');
        final value = fixture.db!.getSync('key');
        expect(value, isNull);
      });

      test('should handle missing key delete', () {
        // Deleting non-existent key should not throw
        expect(() => fixture.db!.delSync('missing/key'), returnsNormally);
      });
    });

    group('batch sync', () {
      test('should execute batch operations', () {
        fixture.db!.batchSync([
          {'type': 'put', 'key': 'users/alice/name', 'value': 'Alice'},
          {'type': 'put', 'key': 'users/bob/name', 'value': 'Bob'},
          {'type': 'del', 'key': 'users/charlie/name'},
        ]);

        expect(fixture.db!.getSync('users/alice/name'), equals('Alice'));
        expect(fixture.db!.getSync('users/bob/name'), equals('Bob'));
        expect(fixture.db!.getSync('users/charlie/name'), isNull);
      });

      test('should validate batch operations', () {
        expect(
          () => fixture.db!.batchSync([
            {'type': 'invalid', 'key': 'key'},
          ]),
          throwsArgumentError,
        );
      });

      test('should require value for put operations', () {
        expect(
          () => fixture.db!.batchSync([
            {'type': 'put', 'key': 'key'},  // missing value
          ]),
          throwsArgumentError,
        );
      });
    });

    group('putObject sync', () {
      test('should flatten object to paths', () {
        fixture.db!.putObjectSync(null, {
          'users': {
            'alice': {'name': 'Alice', 'age': '30'},
          },
        });

        expect(fixture.db!.getSync('users/alice/name'), equals('Alice'));
        expect(fixture.db!.getSync('users/alice/age'), equals('30'));
      });

      test('should handle nested arrays', () {
        fixture.db!.putObjectSync(null, {
          'data': {
            'matrix': [
              [1, 2],
              [3, 4],
            ],
          },
        });

        expect(fixture.db!.getSync('data/matrix/0/0'), equals('1'));
        expect(fixture.db!.getSync('data/matrix/1/1'), equals('4'));
      });

      test('should handle key prefix', () {
        fixture.db!.putObjectSync('users', {
          'alice': {'name': 'Alice'},
        });

        expect(fixture.db!.getSync('users/alice/name'), equals('Alice'));
      });
    });

    group('async operations', () {
      test('should put and get asynchronously', () async {
        await fixture.db!.put('users/alice/name', 'Alice');
        final value = await fixture.db!.get('users/alice/name');
        expect(value, equals('Alice'));
      });

      test('should delete asynchronously', () async {
        await fixture.db!.put('key', 'value');
        await fixture.db!.del('key');
        final value = await fixture.db!.get('key');
        expect(value, isNull);
      });

      test('should batch asynchronously', () async {
        await fixture.db!.batch([
          {'type': 'put', 'key': 'key1', 'value': 'value1'},
          {'type': 'put', 'key': 'key2', 'value': 'value2'},
        ]);

        expect(await fixture.db!.get('key1'), equals('value1'));
        expect(await fixture.db!.get('key2'), equals('value2'));
      });

      test('should putObject asynchronously', () async {
        await fixture.db!.putObject('users', {
          'alice': {'name': 'Alice'},
        });

        expect(await fixture.db!.get('users/alice/name'), equals('Alice'));
      });
    });

    group('getObjectSync', () {
      test('should throw NOT_SUPPORTED for getObject', () {
        expect(
          () => fixture.db!.getObjectSync('key'),
          throwsA(isA<WaveDBException>().having((e) => e.code, 'code', 'NOT_SUPPORTED')),
        );
      });
    });

    group('createReadStream', () {
      test('should iterate over database entries', () async {
        // Insert some test data
        await fixture.db!.put('key1', 'value1');
        await fixture.db!.put('key2', 'value2');
        await fixture.db!.put('key3', 'value3');

        // Iterate over entries
        final stream = fixture.db!.createReadStream();
        final entries = await stream.toList();

        // Verify we got entries back
        expect(entries.length, greaterThanOrEqualTo(3));

        // Verify each entry has a key and value
        for (final entry in entries) {
          expect(entry.key, isNotNull);
          expect(entry.value, isNotNull);
        }
      });

      test('should return empty stream for empty database', () async {
        final stream = fixture.db!.createReadStream();
        final entries = await stream.toList();
        expect(entries, isEmpty);
      });
    });

    group('lifecycle', () {
      test('should report isClosed correctly', () {
        expect(fixture.db!.isClosed, isFalse);
        fixture.db!.close();
        expect(fixture.db!.isClosed, isTrue);
      });

      test('should handle double close', () {
        fixture.db!.close();
        fixture.db!.close(); // Should not throw
        expect(fixture.db!.isClosed, isTrue);
      });

      test('should expose path and delimiter', () {
        expect(fixture.db!.path, equals(fixture.dbPath));
        expect(fixture.db!.delimiter, equals('/'));
      });

      test('should support custom delimiter', () async {
        final customDb = WaveDB(fixture.dbPath + '_custom', delimiter: '.');
        try {
          customDb.putSync('users.alice.name', 'Alice');
          expect(customDb.getSync('users.alice.name'), equals('Alice'));
          expect(customDb.delimiter, equals('.'));
        } finally {
          customDb.close();
          await Directory(customDb.path).delete(recursive: true);
        }
      });
    });
  });

  group('WaveDBException', () {
    test('should create with code and message', () {
      final e = WaveDBException('TEST', 'Test message');
      expect(e.code, equals('TEST'));
      expect(e.message, equals('Test message'));
      expect(e.toString(), equals('WaveDBException(TEST): Test message'));
    });

    test('should create factory methods', () {
      final notFound = WaveDBException.notFound('key');
      expect(notFound.code, equals('NOT_FOUND'));
      expect(notFound.message, contains('key'));

      final closed = WaveDBException.databaseClosed();
      expect(closed.code, equals('DATABASE_CLOSED'));

      final invalid = WaveDBException.invalidPath('bad/path');
      expect(invalid.code, equals('INVALID_PATH'));
    });
  });
}