// benchmark/benchmark.dart
import 'dart:io';
import 'package:wavedb/wavedb.dart';

const int iterations = 10000;
const int warmup = 100;

void cleanPath(String dbPath) {
  final dir = Directory(dbPath);
  if (dir.existsSync()) dir.deleteSync(recursive: true);
}

Future<void> warmupDb(WaveDB db) async {
  for (var i = 0; i < warmup; i++) {
    await db.put('warmup$i', 'value$i');
    await db.get('warmup$i');
  }
}

Future<void> benchSequentialAsync(WaveDB db, String label) async {
  print('\nSequential Async ($label):');
  print('-' * 50);

  var sw = Stopwatch()..start();
  for (var i = 0; i < iterations; i++) {
    await db.put('key$i', 'value$i');
  }
  sw.stop();
  var ops = (iterations / sw.elapsedMilliseconds * 1000).round();
  print('  put:       ${ops.toString().padLeft(8)} ops/sec');

  sw = Stopwatch()..start();
  for (var i = 0; i < iterations; i++) {
    await db.get('key$i');
  }
  sw.stop();
  ops = (iterations / sw.elapsedMilliseconds * 1000).round();
  print('  get:       ${ops.toString().padLeft(8)} ops/sec');
}

Future<void> benchSync(WaveDB db, String label) async {
  print('\nSync Operations ($label):');
  print('-' * 50);

  var sw = Stopwatch()..start();
  for (var i = 0; i < iterations; i++) {
    db.putSync('sync$i', 'value$i');
  }
  sw.stop();
  var ops = (iterations / sw.elapsedMilliseconds * 1000).round();
  print('  putSync:   ${ops.toString().padLeft(8)} ops/sec');

  sw = Stopwatch()..start();
  for (var i = 0; i < iterations; i++) {
    db.getSync('sync$i');
  }
  sw.stop();
  ops = (iterations / sw.elapsedMilliseconds * 1000).round();
  print('  getSync:   ${ops.toString().padLeft(8)} ops/sec');
}

Future<void> benchBatch(WaveDB db) async {
  print('\nBatch Operations:');
  print('-' * 50);
  const batchSize = 1000;
  final batchOps = List.generate(
      batchSize, (i) => {'type': 'put', 'key': 'batch$i', 'value': 'value$i'});
  final batchCount = iterations ~/ batchSize;

  final sw = Stopwatch()..start();
  for (var i = 0; i < batchCount; i++) {
    await db.batch(batchOps);
  }
  sw.stop();
  final ops = (iterations / sw.elapsedMilliseconds * 1000).round();
  print('  batch:    ${ops.toString().padLeft(8)} ops/sec ($batchSize ops/batch)');
}

Future<void> benchConcurrent(WaveDB db, String label) async {
  print('\nConcurrent Async ($label):');
  print('-' * 50);

  final cpuCount = Platform.numberOfProcessors;
  final concurrencies = [1, 2, 4, 8, 16, 32].where((c) => c <= cpuCount * 2).toList();
  if (!concurrencies.contains(1)) concurrencies.insert(0, 1);

  for (final c in concurrencies) {
    // Concurrent puts
    final putKeys = List.generate(iterations, (i) => 'cp${c}_$i');
    var sw = Stopwatch()..start();
    var idx = 0;
    while (idx < iterations) {
      final chunk = putKeys.sublist(idx, (idx + c).clamp(0, iterations));
      await Future.wait(chunk.map((k) => db.put(k, 'v$idx')));
      idx += c;
    }
    sw.stop();
    var ops = (iterations / sw.elapsedMilliseconds * 1000).round();
    print('  put (c=${c.toString().padLeft(2)}):  ${ops.toString().padLeft(8)} ops/sec');

    // Concurrent gets
    sw = Stopwatch()..start();
    idx = 0;
    while (idx < iterations) {
      final chunk = putKeys.sublist(idx, (idx + c).clamp(0, iterations));
      await Future.wait(chunk.map((k) => db.get(k)));
      idx += c;
    }
    sw.stop();
    ops = (iterations / sw.elapsedMilliseconds * 1000).round();
    print('  get (c=${c.toString().padLeft(2)}):  ${ops.toString().padLeft(8)} ops/sec');
  }
}

Future<void> benchStream(WaveDB db) async {
  print('\nStream Operations:');
  print('-' * 50);
  final sw = Stopwatch()..start();
  var count = 0;
  try {
    await for (final _ in db.createReadStream()) {
      count++;
    }
  } catch (_) {}
  sw.stop();
  final ops = count > 0 ? (count / sw.elapsedMilliseconds * 1000).round() : 0;
  print('  stream:    ${ops.toString().padLeft(8)} entries/sec ($count entries)');
}

Future<void> runBenchmarks(
    String dbPath, WaveDBConfig? config, String label) async {
  cleanPath(dbPath);
  final db = WaveDB(dbPath, config: config);
  await warmupDb(db);

  await benchSequentialAsync(db, label);
  await benchSync(db, label);
  await benchBatch(db);
  await benchConcurrent(db, label);
  await benchStream(db);

  db.close();
  cleanPath(dbPath);
}

Future<void> main() async {
  print('WaveDB Dart FFI Benchmarks');
  print('=' * 50);
  print('Dart ${Platform.version}');
  print('Iterations: $iterations');
  print('CPUs: ${Platform.numberOfProcessors}');

  // ---- In-memory (no persistence) ----
  print('\n' + '=' * 50);
  print('MODE: In-Memory (enablePersist=false)');
  print('=' * 50);
  await runBenchmarks('/tmp/wavedb_dart_bench_mem', const WaveDBConfig(enablePersist: false, lruMemoryMb: 50), 'in-memory');

  // ---- DEBOUNCED WAL ----
  print('\n' + '=' * 50);
  print('MODE: DEBOUNCED WAL (100ms fsync)');
  print('=' * 50);
  await runBenchmarks('/tmp/wavedb_dart_bench_debounced', const WaveDBConfig(walSyncMode: 'debounced', walDebounceMs: 100, lruMemoryMb: 50), 'DEBOUNCED');

  // ---- ASYNC WAL ----
  print('\n' + '=' * 50);
  print('MODE: ASYNC WAL (OS cache, no fsync)');
  print('=' * 50);
  await runBenchmarks('/tmp/wavedb_dart_bench_async', const WaveDBConfig(walSyncMode: 'async', lruMemoryMb: 50), 'ASYNC');

  // ---- IMMEDIATE WAL ----
  print('\n' + '=' * 50);
  print('MODE: IMMEDIATE WAL (fsync per write)');
  print('=' * 50);
  await runBenchmarks('/tmp/wavedb_dart_bench_immediate', const WaveDBConfig(walSyncMode: 'immediate', lruMemoryMb: 50), 'IMMEDIATE');

  print('\n' + '=' * 50);
}