// test/library_test.dart
import 'package:test/test.dart';
import 'package:wavedb/src/native/wavedb_library.dart';
import 'package:wavedb/src/exceptions.dart';

void main() {
  group('WaveDBLibrary', () {
    test('should throw WaveDBException when library not found', () {
      // Reset to force reload
      WaveDBLibrary.reset();

      // Set a non-existent path
      WaveDBLibrary.setLibraryPath('/nonexistent/path/libwavedb.so');

      expect(
        () => WaveDBLibrary.load(),
        throwsA(isA<WaveDBException>().having(
          (e) => e.code,
          'code',
          WaveDBException.libraryNotFoundCode,
        )),
      );

      // Reset for other tests
      WaveDBLibrary.reset();
    });

    test('should use custom library path', () {
      WaveDBLibrary.reset();
      WaveDBLibrary.setLibraryPath('/custom/path/libwavedb.so');

      expect(WaveDBLibrary.isLoaded, isFalse);

      WaveDBLibrary.reset();
    });

    test('should report isLoaded correctly', () {
      WaveDBLibrary.reset();
      expect(WaveDBLibrary.isLoaded, isFalse);

      // Note: This will try to load and likely fail without the actual library
      // In integration tests with the library present, this would succeed
      WaveDBLibrary.reset();
    });
  });

  group('WaveDBException', () {
    test('should create exception with code and message', () {
      final e = WaveDBException('TEST_CODE', 'Test message');
      expect(e.code, equals('TEST_CODE'));
      expect(e.message, equals('Test message'));
      expect(e.toString(), equals('WaveDBException(TEST_CODE): Test message'));
    });

    test('should create notFound factory', () {
      final e = WaveDBException.notFound('users/alice');
      expect(e.code, equals(WaveDBException.notFoundCode));
      expect(e.message, contains('users/alice'));
    });

    test('should create invalidPath factory', () {
      final e = WaveDBException.invalidPath('bad/path', 'empty segment');
      expect(e.code, equals(WaveDBException.invalidPathCode));
      expect(e.message, contains('bad/path'));
      expect(e.message, contains('empty segment'));
    });

    test('should create databaseClosed factory', () {
      final e = WaveDBException.databaseClosed();
      expect(e.code, equals(WaveDBException.databaseClosedCode));
      expect(e.message, equals('Database is closed'));
    });

    test('should create ioError factory', () {
      final e = WaveDBException.ioError('write', 'disk full');
      expect(e.code, equals(WaveDBException.ioErrorCode));
      expect(e.message, contains('write failed'));
      expect(e.message, contains('disk full'));
    });
  });
}
