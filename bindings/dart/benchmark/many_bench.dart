import 'dart:io';
import 'package:wavedb/wavedb.dart';

// Benchmark for the putMany / getMany / deleteMany helpers.
// Run with: dart benchmark/many_bench.dart 2>/dev/null
// (stderr is silenced because the C library floods INFO logs on every MVCC
// access, which otherwise buries the results.)

const int iterations = 5000;
const int batchSize = 1000;

void report(String label, int ops, int elapsedMicros) {
  final elapsedS = elapsedMicros / 1e6;
  final opsPerSec = ops / elapsedS;
  final usPerOp = elapsedMicros / ops;
  print('  ${label.padRight(35)} '
      '${opsPerSec.round().toString().padLeft(10)} ops/sec  '
      '${usPerOp.toStringAsFixed(2).padLeft(7)} us/op');
}

Future<void> main() async {
  final base = Directory.systemTemp.createTempSync('wavedb_bm_');
  final dbPath = '${base.path}/mem';
  final db = WaveDB(
    dbPath,
    config: WaveDBConfig(enablePersist: false, walSyncMode: 'async'),
  );

  for (var i = 0; i < 100; i++) {
    await db.put('warm$i', 'v$i');
  }

  print('Async WAL, ITERATIONS=$iterations, BATCH_SIZE=$batchSize');
  print('=' * 70);

  var sw = Stopwatch()..start();
  for (var i = 0; i < iterations; i++) {
    await db.put('put_ind$i', 'v$i');
  }
  sw.stop();
  report('individual await put()', iterations, sw.elapsedMicroseconds);

  final items = List.generate(batchSize, (i) => ('put_many$i', 'v$i'));
  final batches = iterations ~/ batchSize;
  sw = Stopwatch()..start();
  for (var i = 0; i < batches; i++) {
    await db.putMany(items);
  }
  sw.stop();
  report('putMany ($batchSize/batch)', iterations, sw.elapsedMicroseconds);

  final ops = List.generate(
    batchSize,
    (i) => {'type': 'put', 'key': 'batch$i', 'value': 'v$i'},
  );
  sw = Stopwatch()..start();
  for (var i = 0; i < batches; i++) {
    await db.batch(ops);
  }
  sw.stop();
  report('batch ($batchSize/batch)', iterations, sw.elapsedMicroseconds);

  final keys = List.generate(iterations, (i) => 'put_ind$i');
  sw = Stopwatch()..start();
  for (var i = 0; i < iterations; i++) {
    await db.get(keys[i]);
  }
  sw.stop();
  report('individual await get()', iterations, sw.elapsedMicroseconds);

  sw = Stopwatch()..start();
  for (var i = 0; i < iterations; i += batchSize) {
    await db.getMany(keys.sublist(i, i + batchSize));
  }
  sw.stop();
  report('getMany ($batchSize/batch)', iterations, sw.elapsedMicroseconds);

  final delKeys = List.generate(batchSize, (i) => 'put_many$i');
  sw = Stopwatch()..start();
  for (var i = 0; i < batches; i++) {
    await db.deleteMany(delKeys);
  }
  sw.stop();
  report('deleteMany ($batchSize/batch)', iterations, sw.elapsedMicroseconds);

  // db.close() hangs in this build; exit() is the workaround. See #3 in the
  // commit that added this file.
  exit(0);
}