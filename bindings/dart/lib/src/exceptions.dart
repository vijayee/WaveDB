// lib/src/exceptions.dart

/// WaveDB exception with error code
class WaveDBException implements Exception {
  final String code;
  final String message;

  WaveDBException(this.code, this.message);

  @override
  String toString() => 'WaveDBException($code): $message';

  // Error codes
  static const String notFoundCode = 'NOT_FOUND';
  static const String invalidPathCode = 'INVALID_PATH';
  static const String ioErrorCode = 'IO_ERROR';
  static const String databaseClosedCode = 'DATABASE_CLOSED';
  static const String invalidArgumentCode = 'INVALID_ARGUMENT';
  static const String notSupportedCode = 'NOT_SUPPORTED';
  static const String corruptionCode = 'CORRUPTION';
  static const String conflictCode = 'CONFLICT';
  static const String libraryNotFoundCode = 'LIBRARY_NOT_FOUND';
  static const String unsupportedPlatformCode = 'UNSUPPORTED_PLATFORM';

  // Factory constructors
  factory WaveDBException.notFound([String? key]) {
    return WaveDBException(notFoundCode, key != null ? 'Key not found: $key' : 'Key not found');
  }

  factory WaveDBException.invalidPath(String path, [String? reason]) {
    return WaveDBException(
      invalidPathCode,
      reason != null ? 'Invalid path "$path": $reason' : 'Invalid path: $path',
    );
  }

  factory WaveDBException.ioError(String operation, [String? details]) {
    return WaveDBException(
      ioErrorCode,
      details != null ? '$operation failed: $details' : '$operation failed',
    );
  }

  factory WaveDBException.databaseClosed() {
    return WaveDBException(databaseClosedCode, 'Database is closed');
  }

  factory WaveDBException.invalidArgument(String message) {
    return WaveDBException(invalidArgumentCode, message);
  }

  factory WaveDBException.notSupported(String operation) {
    return WaveDBException(notSupportedCode, 'Operation not supported: $operation');
  }

  factory WaveDBException.libraryNotFound(String message) {
    return WaveDBException(libraryNotFoundCode, message);
  }

  factory WaveDBException.unsupportedPlatform(String message) {
    return WaveDBException(unsupportedPlatformCode, message);
  }

  factory WaveDBException.corruption(String message) {
    return WaveDBException(corruptionCode, message);
  }

  factory WaveDBException.conflict(String message) {
    return WaveDBException(conflictCode, message);
  }
}
