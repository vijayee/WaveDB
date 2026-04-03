// lib/src/native/wavedb_library.dart
import 'dart:io';
import 'dart:ffi';
import '../exceptions.dart';

/// Platform-specific library loader for WaveDB native library
class WaveDBLibrary {
  static DynamicLibrary? _lib;
  static String? _libPath;

  /// Set custom library path (useful for bundled deployments)
  static void setLibraryPath(String path) {
    _libPath = path;
    _lib = null; // Force reload
  }

  /// Load the native library
  static DynamicLibrary load() {
    if (_lib != null) return _lib!;

    // Try custom path first
    if (_libPath != null) {
      _lib = DynamicLibrary.open(_libPath!);
      return _lib!;
    }

    // Platform-specific loading
    if (Platform.isLinux) {
      _lib = _loadLinux();
    } else if (Platform.isMacOS) {
      _lib = _loadMacOS();
    } else if (Platform.isWindows) {
      _lib = _loadWindows();
    } else {
      throw WaveDBException.unsupportedPlatform(
        'WaveDB is not supported on ${Platform.operatingSystem}',
      );
    }

    return _lib!;
  }

  static DynamicLibrary _loadLinux() {
    final paths = [
      'libwavedb.so',
      './libwavedb.so',
      '/usr/local/lib/libwavedb.so',
      '/usr/lib/libwavedb.so',
    ];
    return _tryPaths(paths, 'libwavedb.so');
  }

  static DynamicLibrary _loadMacOS() {
    final paths = [
      'libwavedb.dylib',
      './libwavedb.dylib',
      '/usr/local/lib/libwavedb.dylib',
    ];
    return _tryPaths(paths, 'libwavedb.dylib');
  }

  static DynamicLibrary _loadWindows() {
    final paths = [
      'wavedb.dll',
      '.\\wavedb.dll',
    ];
    return _tryPaths(paths, 'wavedb.dll');
  }

  static DynamicLibrary _tryPaths(List<String> paths, String libName) {
    final errors = <String>[];

    for (final path in paths) {
      try {
        return DynamicLibrary.open(path);
      } catch (e) {
        errors.add('$path: $e');
      }
    }

    throw WaveDBException.libraryNotFound(
      '$libName not found. Tried:\n${errors.join('\n')}',
    );
  }

  /// Check if the library is loaded
  static bool get isLoaded => _lib != null;

  /// Reset the library (for testing)
  static void reset() {
    _lib = null;
    _libPath = null;
  }
}
