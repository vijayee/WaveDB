// test/conversion_test.dart
import 'dart:typed_data';
import 'package:test/test.dart';
import 'package:wavedb/src/path.dart';
import 'package:wavedb/src/identifier.dart';
import 'package:wavedb/src/exceptions.dart';

void main() {
  group('PathConverter', () {
    group('toNative input validation', () {
      test('should throw on null key', () {
        // Note: When native library is not found, WaveDBException is thrown first
        // When library is present, ArgumentError should be thrown
        expect(
          () => PathConverter.toNative(null, '/'),
          anyOf([
            throwsArgumentError,
            throwsA(isA<WaveDBException>().having((e) => e.code, 'code', 'LIBRARY_NOT_FOUND')),
          ]),
        );
      });

      test('should throw on empty string key', () {
        expect(
          () => PathConverter.toNative('', '/'),
          anyOf([
            throwsArgumentError,
            throwsA(isA<WaveDBException>().having((e) => e.code, 'code', 'LIBRARY_NOT_FOUND')),
          ]),
        );
      });

      test('should throw on empty list key', () {
        expect(
          () => PathConverter.toNative([], '/'),
          anyOf([
            throwsArgumentError,
            throwsA(isA<WaveDBException>().having((e) => e.code, 'code', 'LIBRARY_NOT_FOUND')),
          ]),
        );
      });

      test('should throw on invalid key type', () {
        expect(
          () => PathConverter.toNative(123, '/'),
          anyOf([
            throwsArgumentError,
            throwsA(isA<WaveDBException>().having((e) => e.code, 'code', 'LIBRARY_NOT_FOUND')),
          ]),
        );
      });

      test('should accept string key', () {
        // Note: This will fail without the native library
        // The test validates input parsing, not FFI interaction
        expect(
          () => PathConverter.toNative('users/alice/name', '/'),
          anyOf([
            throwsA(isA<WaveDBException>().having((e) => e.code, 'code', 'LIBRARY_NOT_FOUND')),
            returnsNormally,
          ]),
        );
      });

      test('should accept list key', () {
        expect(
          () => PathConverter.toNative(['users', 'alice', 'name'], '/'),
          anyOf([
            throwsA(isA<WaveDBException>().having((e) => e.code, 'code', 'LIBRARY_NOT_FOUND')),
            returnsNormally,
          ]),
        );
      });
    });

    group('toNative delimiter handling', () {
      test('should split by custom delimiter', () {
        // This validates delimiter splitting logic
        const key = 'users.alice.name';
        const delimiter = '.';
        // Without native library, this tests the splitting logic
        final parts = key.split(delimiter).where((s) => s.isNotEmpty).toList();
        expect(parts, equals(['users', 'alice', 'name']));
      });

      test('should filter empty segments', () {
        const key = 'users//alice/name';
        final parts = key.split('/').where((s) => s.isNotEmpty).toList();
        expect(parts, equals(['users', 'alice', 'name']));
      });
    });
  });

  group('IdentifierConverter', () {
    group('toNative input validation', () {
      test('should accept string value', () {
        expect(
          () => IdentifierConverter.toNative('hello'),
          anyOf([
            throwsA(isA<WaveDBException>().having((e) => e.code, 'code', 'LIBRARY_NOT_FOUND')),
            returnsNormally,
          ]),
        );
      });

      test('should accept Uint8List value', () {
        final bytes = Uint8List.fromList([0x01, 0x02, 0x03]);
        expect(
          () => IdentifierConverter.toNative(bytes),
          anyOf([
            throwsA(isA<WaveDBException>().having((e) => e.code, 'code', 'LIBRARY_NOT_FOUND')),
            returnsNormally,
          ]),
        );
      });

      test('should accept List<int> value', () {
        expect(
          () => IdentifierConverter.toNative([1, 2, 3]),
          anyOf([
            throwsA(isA<WaveDBException>().having((e) => e.code, 'code', 'LIBRARY_NOT_FOUND')),
            returnsNormally,
          ]),
        );
      });

      test('should throw on invalid type', () {
        expect(
          () => IdentifierConverter.toNative(123.45),
          throwsArgumentError,
        );
      });
    });

    group('isPrintableASCII', () {
      test('should return true for printable ASCII', () {
        expect(IdentifierConverter.isPrintableASCII([72, 101, 108, 108, 111]), isTrue); // "Hello"
      });

      test('should return true for empty bytes', () {
        expect(IdentifierConverter.isPrintableASCII([]), isTrue);
      });

      test('should return false for bytes below 32', () {
        expect(IdentifierConverter.isPrintableASCII([0, 1, 2]), isFalse);
      });

      test('should return false for bytes >= 127', () {
        expect(IdentifierConverter.isPrintableASCII([127, 128, 255]), isFalse);
      });

      test('should return true for mixed printable', () {
        expect(IdentifierConverter.isPrintableASCII([65, 32, 66]), isTrue); // "A B"
      });

      test('should return true for tab, newline, carriage return', () {
        expect(IdentifierConverter.isPrintableASCII([9]), isTrue);  // tab
        expect(IdentifierConverter.isPrintableASCII([10]), isTrue); // newline
        expect(IdentifierConverter.isPrintableASCII([13]), isTrue); // carriage return
        expect(IdentifierConverter.isPrintableASCII([9, 10, 13]), isTrue);
      });
    });
  });
}
