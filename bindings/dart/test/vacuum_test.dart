// test/vacuum_test.dart
//
// Vacuum / compaction binding test for the Dart FFI binding.
//
// Verifies:
//   - The `vacuumConfig` field on WaveDBConfig is wired through to the
//     native database_config_t.vacuum_config struct.
//   - WaveDB.vacuum() compacts the page file (file size shrinks).
//   - Keys remain readable after vacuum.
//
// Learnings carried over from Task 17 (Node.js binding):
//   - The path passed to WaveDB is a directory; the C layer appends
//     /data.wdbp internally.
//   - In sync_only mode, database_flush_dirty_bnodes must be called before
//     vacuum for the page file to actually have stale bytes to reclaim.
//     WaveDB.flush() exposes this.
//   - Keys must span multiple 4 KiB pages so overwriting them leaves real
//     stale CoW bytes behind for vacuum to reclaim.

import 'dart:io';
import 'package:test/test.dart';
import 'package:wavedb/wavedb.dart';

void main() {
  late Directory tmpdir;
  late WaveDB db;

  setUp(() {
    tmpdir = Directory.systemTemp.createTempSync('wavedb_dart_vacuum_');
    // location is a directory — database_create_with_config appends
    // /data.wdbp internally.
    db = WaveDB(
      tmpdir.path,
      config: WaveDBConfig(
        syncOnly: true,
        vacuumConfig: VacuumConfig(
          mode: VacuumMode.manualOnly,
          minFileSizeBytes: 0,
          minStaleBytes: 0,
        ),
      ),
    );
  });

  tearDown(() {
    db.close();
    try {
      tmpdir.deleteSync(recursive: true);
    } catch (_) {
      // Ignore cleanup errors
    }
  });

  test('vacuum shrinks the page file after overwrites and keeps keys readable', () {
    // Make values large enough to span multiple 4 KiB pages so vacuum has
    // real stale CoW bytes to reclaim. ~512 bytes per value × 200 keys
    // produces enough overwritten pages.
    final big = List<String>.generate(200, (i) => 'v0_${'x' * 512}_$i');

    for (var i = 0; i < 200; i++) {
      db.putSync('k/$i', big[i]);
    }
    // Flush so the page file actually has data on disk.
    db.flush();

    // Overwrite each key 5 times, flushing after each pass so each
    // overwrite leaves stale CoW copies in the page file.
    for (var rep = 1; rep <= 5; rep++) {
      for (var i = 0; i < 200; i++) {
        db.putSync('k/$i', 'v${rep}_${'x' * 512}_$i');
      }
      db.flush();
    }

    final pageFile = File('${tmpdir.path}/data.wdbp');
    expect(pageFile.existsSync(), isTrue,
        reason: 'page file data.wdbp should exist after flush');
    final before = pageFile.lengthSync();
    expect(before, greaterThan(0));

    // Manual vacuum — must be exposed on the Dart wrapper.
    expect(db.vacuum(), 0);

    final after = pageFile.lengthSync();
    expect(after, lessThan(before),
        reason: 'page file should shrink after vacuum: $before -> $after');

    // Keys must still be readable after vacuum.
    for (var i = 0; i < 200; i++) {
      final v = db.getSync('k/$i');
      expect(v, equals('v5_${'x' * 512}_$i'),
          reason: 'key k/$i should survive vacuum');
    }
  });

  test('vacuumConfig defaults are accepted without error', () {
    // Just constructing with a default VacuumConfig should not throw.
    final dir = Directory.systemTemp.createTempSync('wavedb_dart_vacuum_defaults_');
    try {
      final db2 = WaveDB(
        dir.path,
        config: WaveDBConfig(
          syncOnly: true,
          vacuumConfig: VacuumConfig(),
        ),
      );
      db2.putSync('k', 'v');
      db2.flush();
      expect(db2.vacuum(), 0);
      expect(db2.getSync('k'), 'v');
      db2.close();
    } finally {
      try {
        dir.deleteSync(recursive: true);
      } catch (_) {}
    }
  });
}