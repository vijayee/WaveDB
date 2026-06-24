import 'dart:io';
import 'package:wavedb/wavedb.dart';

// Focused async-WAL benchmark matching the Dart README's stated config.
// Run with: dart benchmark/async_bench.dart 2>/dev/null

const int iterations = 1000;

void report(String label, int ops, int elapsedMicros) {
  final elapsedS = elapsedMicros / 1e6;
  final opsPerSec = ops / elapsedS;
  print('  ${label.padRight(20)} ${opsPerSec.round().toString().padLeft(8)} ops/sec');
}

Future<void> main() async {
  final base = Directory.systemTemp.createTempSync('wavedb_async_');
  final dbPath = '${base.path}/db';
  final db = WaveDB(
    dbPath,
    config: WaveDBConfig(
      enablePersist: false,
      walSyncMode: 'async',
      lruMemoryMb: 50,
      workerThreads: 32,
    ),
  );

  for (var i = 0; i < 100; i++) {
    await db.put('warm$i', 'v$i');
  }

  print('Async WAL, 32 worker threads, ITERATIONS=$iterations');
  print('=' * 60);

  // Sync ops
  var sw = Stopwatch()..start();
  for (var i = 0; i < iterations; i++) {
    db.putSync('sync$i', 'v$i');
  }
  sw.stop();
  report('putSync', iterations, sw.elapsedMicroseconds);

  sw = Stopwatch()..start();
  for (var i = 0; i < iterations; i++) {
    db.getSync('sync$i');
  }
  sw.stop();
  report('getSync', iterations, sw.elapsedMicroseconds);

  // Concurrent put/get
  for (final c in [1, 2, 4, 8, 16, 32]) {
    final keys = List.generate(iterations, (i) => 'cp${c}_$i');
    sw = Stopwatch()..start();
    for (var i = 0; i < iterations; i += c) {
      final chunk = keys.sublist(i, i + c);
      await Future.wait(chunk.asMap().entries.map(
        (e) => db.put(e.value, 'v${e.key}'),
      ));
    }
    sw.stop();
    report('put (c=$c)', iterations, sw.elapsedMicroseconds);

    sw = Stopwatch()..start();
    for (var i = 0; i < iterations; i += c) {
      final chunk = keys.sublist(i, i + c);
      await Future.wait(chunk.map((k) => db.get(k)));
    }
    sw.stop();
    report('get (c=$c)', iterations, sw.elapsedMicroseconds);
  }

  db.close();
}