// benchmark/benchmark.dart
import 'dart:io';
import 'dart:async';
import 'package:wavedb/wavedb.dart';

const int iterations = 100;
const int warmup = 10;
const String dbPath = '/tmp/wavedb_benchmark_dart';

Future<void> main() async {
  // Clean up previous benchmark
  final dir = Directory(dbPath);
  if (await dir.exists()) {
    await dir.delete(recursive: true);
  }

  final db = WaveDB(dbPath);

  print('WaveDB Performance Benchmarks');
  print('=' * 50);
  print('Dart ${Platform.version}');
  print('Iterations: $iterations');
  print('');

  // Warmup
  for (var i = 0; i < warmup; i++) {
    await db.put('warmup$i', 'value$i');
    await db.get('warmup$i');
  }

  // Async Put
  print('Async Operations:');
  print('-' * 50);
  var stopwatch = Stopwatch()..start();
  for (var i = 0; i < iterations; i++) {
    await db.put('key$i', 'value$i');
  }
  stopwatch.stop();
  var elapsed = stopwatch.elapsedMilliseconds / 1000.0;
  var ops = (iterations / elapsed).round();
  print('  put:       ${ops.toString().padLeft(8)} ops/sec (${(elapsed * 1000).toStringAsFixed(2)}ms)');

  // Async Get
  stopwatch..reset()..start();
  for (var i = 0; i < iterations; i++) {
    await db.get('key$i');
  }
  stopwatch.stop();
  elapsed = stopwatch.elapsedMilliseconds / 1000.0;
  ops = (iterations / elapsed).round();
  print('  get:       ${ops.toString().padLeft(8)} ops/sec (${(elapsed * 1000).toStringAsFixed(2)}ms)');

  // Sync Put
  print('\nSync Operations:');
  print('-' * 50);
  stopwatch..reset()..start();
  for (var i = 0; i < iterations; i++) {
    db.putSync('sync$i', 'value$i');
  }
  stopwatch.stop();
  elapsed = stopwatch.elapsedMilliseconds / 1000.0;
  ops = (iterations / elapsed).round();
  print('  putSync:   ${ops.toString().padLeft(8)} ops/sec (${(elapsed * 1000).toStringAsFixed(2)}ms)');

  // Sync Get
  stopwatch..reset()..start();
  for (var i = 0; i < iterations; i++) {
    db.getSync('sync$i');
  }
  stopwatch.stop();
  elapsed = stopwatch.elapsedMilliseconds / 1000.0;
  ops = (iterations / elapsed).round();
  print('  getSync:   ${ops.toString().padLeft(8)} ops/sec (${(elapsed * 1000).toStringAsFixed(2)}ms)');

  // Batch operations
  print('\nBatch Operations:');
  print('-' * 50);
  const batchSize = 1000;
  final batchOps = <Map<String, dynamic>>[];
  for (var i = 0; i < batchSize; i++) {
    batchOps.add({'type': 'put', 'key': 'batch$i', 'value': 'value$i'});
  }

  stopwatch..reset()..start();
  for (var i = 0; i < iterations ~/ batchSize; i++) {
    await db.batch(batchOps);
  }
  stopwatch.stop();
  elapsed = stopwatch.elapsedMilliseconds / 1000.0;
  ops = (iterations / elapsed).round();
  print('  batch:     ${ops.toString().padLeft(8)} ops/sec (${(elapsed * 1000).toStringAsFixed(2)}ms, $batchSize ops/batch)');

  // Stream iteration (if supported)
  print('\nStream Operations:');
  print('-' * 50);
  try {
    var count = 0;
    stopwatch..reset()..start();

    await for (final _ in db.createReadStream()) {
      count++;
    }

    stopwatch.stop();
    elapsed = stopwatch.elapsedMilliseconds / 1000.0;
    ops = count > 0 ? (count / elapsed).round() : 0;
    print('  stream:    ${ops.toString().padLeft(8)} entries/sec ($count entries in ${(elapsed * 1000).toStringAsFixed(2)}ms)');
  } catch (e) {
    print('  stream:    NOT_SUPPORTED (database_scan API not available)');
  }

  print('\n' + '=' * 50);
  db.close();

  // Cleanup
  if (await dir.exists()) {
    await dir.delete(recursive: true);
  }
}