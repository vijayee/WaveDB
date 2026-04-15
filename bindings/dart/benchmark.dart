import 'dart:io';
import 'dart:isolate';
import 'package:wavedb/wavedb.dart';

void main() async {
  final dbPath = '/tmp/wavedb_dart_benchmark';
  final dir = Directory(dbPath);
  if (dir.existsSync()) dir.deleteSync(recursive: true);

  final db = WaveDB(dbPath);
  const iterations = 10000;

  print('WaveDB Dart FFI Benchmarks');
  print('=' * 50);
  print('Dart ${Platform.version}');
  print('Iterations: $iterations');
  print('');

  // Warmup
  for (var i = 0; i < 100; i++) {
    db.putSync('warmup$i', 'value$i');
    db.getSync('warmup$i');
  }

  // Async Put
  print('Async Operations:');
  print('-' * 50);
  var sw = Stopwatch()..start();
  for (var i = 0; i < iterations; i++) {
    await db.put('akey$i', 'value$i');
  }
  sw.stop();
  var ops = (iterations / sw.elapsedMilliseconds * 1000).round();
  print('  put:       ${ops.toString().padLeft(8)} ops/sec (${sw.elapsedMilliseconds}ms)');

  // Async Get
  sw = Stopwatch()..start();
  for (var i = 0; i < iterations; i++) {
    await db.get('akey$i');
  }
  sw.stop();
  ops = (iterations / sw.elapsedMilliseconds * 1000).round();
  print('  get:       ${ops.toString().padLeft(8)} ops/sec (${sw.elapsedMilliseconds}ms)');

  // Sync Put
  print('\nSync Operations:');
  print('-' * 50);
  sw = Stopwatch()..start();
  for (var i = 0; i < iterations; i++) {
    db.putSync('skey$i', 'value$i');
  }
  sw.stop();
  ops = (iterations / sw.elapsedMilliseconds * 1000).round();
  print('  putSync:   ${ops.toString().padLeft(8)} ops/sec (${sw.elapsedMilliseconds}ms)');

  // Sync Get
  sw = Stopwatch()..start();
  for (var i = 0; i < iterations; i++) {
    db.getSync('skey$i');
  }
  sw.stop();
  ops = (iterations / sw.elapsedMilliseconds * 1000).round();
  print('  getSync:   ${ops.toString().padLeft(8)} ops/sec (${sw.elapsedMilliseconds}ms)');

  // Batch
  print('\nBatch Operations:');
  print('-' * 50);
  const batchSize = 1000;
  final batchOps = List.generate(batchSize, (i) => {'type': 'put', 'key': 'bkey$i', 'value': 'value$i'});
  final batchCount = iterations ~/ batchSize;

  sw = Stopwatch()..start();
  for (var i = 0; i < batchCount; i++) {
    await db.batch(batchOps);
  }
  sw.stop();
  ops = (iterations / sw.elapsedMilliseconds * 1000).round();
  print('  batch:    ${ops.toString().padLeft(8)} ops/sec (${sw.elapsedMilliseconds}ms, $batchSize ops/batch)');

  print('\n' + '=' * 50);
  db.close();

  if (dir.existsSync()) dir.deleteSync(recursive: true);
}