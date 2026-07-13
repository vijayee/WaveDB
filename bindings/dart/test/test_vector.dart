// test/test_vector.dart
//
// Tests for the VectorLayer Dart FFI binding. Mirrors the gtest coverage
// in tests/test_vector.cpp and the Python tests in
// bindings/python/tests/test_vector.py via the wrapper API in
// lib/src/vector_layer.dart. Covers:
//   - lifecycle (openSeparate / open shared / close / count)
//   - Format/Runtime split + reconfigure (Runtime only)
//   - error handling (invalid dim, empty index name, closed layer)
//   - FLAT insert / search / delete / metadata
//   - IVF insert / train / rebuild / search
//   - SLSH insert / train / rebuild / search (bidirectional scan)
//   - recall sanity (top-1 contains the query vector itself)
//
// Async API (promise_t*) is deferred — bridging the C promise callback to
// a Dart Future needs a NativeCallable.listener trampoline (the
// graph_layer.dart pattern). The sync API is wired + tested here.
import 'dart:io';
import 'dart:typed_data';

import 'package:test/test.dart';
import 'package:wavedb/wavedb.dart';

// ---- helpers ----

VectorFormat _flatFormat({int dim = 4, int distance = Distance.l2}) =>
    VectorFormat(indexType: IndexType.flat, dim: dim, distance: distance);

VectorFormat _ivfFormat({int dim = 4, int nClusters = 3}) => VectorFormat(
      indexType: IndexType.ivf,
      dim: dim,
      distance: Distance.l2,
      ivfNClusters: nClusters,
    );

VectorFormat _slshFormat({int dim = 4}) => VectorFormat(
      indexType: IndexType.slsh,
      dim: dim,
      distance: Distance.l2,
      slshLshTables: 2,
      slshHashBits: 8,
      slshBucketWidth: 1.0,
    );

const VectorRuntime _rt = VectorRuntime(syncOnly: 1);

List<double> _v(List<double> xs) => xs;

void main() {
  late Directory tempDir;

  setUp(() async {
    tempDir = await Directory.systemTemp.createTemp('wavedb_dart_vector_');
  });

  tearDown(() async {
    try {
      await tempDir.delete(recursive: true);
    } catch (_) {}
  });

  // ---- lifecycle ----

  group('lifecycle', () {
    test('openSeparate and count == 0', () {
      final vl = VectorLayer.openSeparate(
          '${tempDir.path}/vltest', 'test', _flatFormat(), _rt);
      expect(vl.count(), 0);
      vl.close();
    });

    test('openSeparate is reusable after reopen (persistence)', () {
      final path = '${tempDir.path}/persist';
      final vl = VectorLayer.openSeparate(path, 'test', _flatFormat(), _rt);
      vl.insertSync('a', _v([1, 0, 0, 0]));
      expect(vl.count(), 1);
      vl.close();

      final vl2 = VectorLayer.openSeparate(path, 'test', _flatFormat(), _rt);
      expect(vl2.count(), 1);
      vl2.close();
    });

    test('open shared on existing db', () {
      final db = WaveDB('${tempDir.path}/db');
      final vl = VectorLayer.open('test', db, _flatFormat(distance: Distance.cosine), _rt);
      expect(vl.count(), 0);
      vl.close();
      db.close();
    });

    test('open with subtree prefixes keys under subtree', () {
      final db = WaveDB('${tempDir.path}/db');
      final st = db.openSubtree('vec');
      final vl = VectorLayer.open('test', db, _flatFormat(), _rt, subtree: st);
      expect(vl.count(), 0);
      vl.close();
      st.close();
      db.close();
    });

    test('double close is a no-op', () {
      final vl = VectorLayer.openSeparate(
          '${tempDir.path}/vltest', 'test', _flatFormat(), _rt);
      vl.close();
      vl.close(); // no throw
    });

    test('operations after close throw StateError', () {
      final vl = VectorLayer.openSeparate(
          '${tempDir.path}/vltest', 'test', _flatFormat(), _rt);
      vl.close();
      expect(() => vl.count(), throwsA(isA<StateError>()));
      expect(() => vl.insertSync('a', _v([1, 2, 3, 4])),
          throwsA(isA<StateError>()));
    });

    test('empty index name is rejected', () {
      expect(
        () => VectorLayer.openSeparate(
            '${tempDir.path}/vltest', '', _flatFormat(), _rt),
        throwsA(isA<VectorLayerException>()),
      );
    });
  });

  // ---- reconfigure (Runtime only) ----

  group('reconfigure', () {
    test('reconfigure accepts Runtime only', () {
      final vl = VectorLayer.openSeparate(
          '${tempDir.path}/vltest', 'test', _flatFormat(), _rt);
      expect(vl.reconfigure(const VectorRuntime(topK: 5, ivfNprobe: 4)), 0);
      vl.close();
    });
  });

  // ---- FLAT ----

  group('FLAT', () {
    test('insert + count', () {
      final vl = VectorLayer.openSeparate(
          '${tempDir.path}/vltest', 'test', _flatFormat(), _rt);
      vl.insertSync('a', _v([1, 0, 0, 0]));
      vl.insertSync('b', _v([0, 1, 0, 0]));
      vl.insertSync('c', _v([0, 0, 1, 0]));
      expect(vl.count(), 3);
      vl.close();
    });

    test('search returns nearest first', () {
      final vl = VectorLayer.openSeparate(
          '${tempDir.path}/vltest', 'test', _flatFormat(), _rt);
      vl.insertSync('a', _v([1, 0, 0, 0]));
      vl.insertSync('b', _v([0, 1, 0, 0]));
      vl.insertSync('c', _v([0, 0, 1, 0]));
      vl.insertSync('d', _v([0, 0, 0, 1]));

      final results = vl.searchSync(_v([1, 0, 0, 0]), 2);
      expect(results, hasLength(2));
      expect(results.first.id, 'a');
      expect(results.first.distance, lessThanOrEqualTo(results.last.distance));
      vl.close();
    });

    test('metadata roundtrip', () {
      final vl = VectorLayer.openSeparate(
          '${tempDir.path}/vltest', 'test', _flatFormat(), _rt);
      vl.insertSync('a', _v([1, 0, 0, 0]), metadata: [1, 2, 3, 4]);
      final results = vl.searchSync(_v([1, 0, 0, 0]), 1);
      expect(results, hasLength(1));
      expect(results.first.id, 'a');
      expect(results.first.metadata, Uint8List.fromList([1, 2, 3, 4]));
      vl.close();
    });

    test('metadata empty when none supplied', () {
      final vl = VectorLayer.openSeparate(
          '${tempDir.path}/vltest', 'test', _flatFormat(), _rt);
      vl.insertSync('a', _v([1, 0, 0, 0]));
      final results = vl.searchSync(_v([1, 0, 0, 0]), 1);
      expect(results.first.metadata, isEmpty);
      vl.close();
    });

    test('delete reduces count and removes from search', () {
      final vl = VectorLayer.openSeparate(
          '${tempDir.path}/vltest', 'test', _flatFormat(), _rt);
      vl.insertSync('a', _v([1, 0, 0, 0]));
      vl.insertSync('b', _v([0, 1, 0, 0]));
      expect(vl.count(), 2);

      vl.deleteSync('a');
      expect(vl.count(), 1);

      final results = vl.searchSync(_v([1, 0, 0, 0]), 5);
      expect(results.where((r) => r.id == 'a'), isEmpty);
      vl.close();
    });

    test('recall: top-1 contains the query vector itself', () {
      final vl = VectorLayer.openSeparate(
          '${tempDir.path}/vltest', 'test', _flatFormat(dim: 4), _rt);
      final vectors = <String, List<double>>{
        'a': [1, 0, 0, 0],
        'b': [0, 1, 0, 0],
        'c': [0, 0, 1, 0],
        'd': [0, 0, 0, 1],
        'e': [1, 1, 0, 0],
      };
      vectors.forEach((id, v) => vl.insertSync(id, v));
      var hits = 0;
      vectors.forEach((id, v) {
        final r = vl.searchSync(v, 1);
        if (r.isNotEmpty && r.first.id == id) hits++;
      });
      expect(hits, vectors.length);
      vl.close();
    });

    test('train + rebuild are no-ops on FLAT (rc 0)', () {
      final vl = VectorLayer.openSeparate(
          '${tempDir.path}/vltest', 'test', _flatFormat(), _rt);
      vl.insertSync('a', _v([1, 0, 0, 0]));
      expect(vl.train(), 0);
      expect(vl.rebuild(), 0);
      expect(vl.count(), 1);
      vl.close();
    });
  });

  // ---- IVF ----

  group('IVF', () {
    test('insert + count', () {
      final vl = VectorLayer.openSeparate(
          '${tempDir.path}/vltest', 'test', _ivfFormat(), _rt);
      vl.insertSync('a', _v([1, 0, 0, 0]));
      vl.insertSync('b', _v([0, 1, 0, 0]));
      vl.insertSync('c', _v([0, 0, 1, 0]));
      expect(vl.count(), 3);
      vl.close();
    });

    test('search after train returns results', () {
      final vl = VectorLayer.openSeparate(
          '${tempDir.path}/vltest', 'test', _ivfFormat(), _rt);
      vl.insertSync('a', _v([1, 0, 0, 0]));
      vl.insertSync('b', _v([0.9, 0.1, 0, 0]));
      vl.insertSync('c', _v([0, 1, 0, 0]));
      vl.insertSync('d', _v([0, 0, 1, 0]));
      expect(vl.train(), 0);

      final results = vl.searchSync(_v([1, 0, 0, 0]), 2);
      expect(results, hasLength(2));
      // nearest neighbor of [1,0,0,0] should be 'a' itself.
      expect(results.first.id, 'a');
      vl.close();
    });

    test('train + rebuild preserve count', () {
      final vl = VectorLayer.openSeparate(
          '${tempDir.path}/vltest', 'test', _ivfFormat(), _rt);
      vl.insertSync('a', _v([1, 0, 0, 0]));
      vl.insertSync('b', _v([0, 1, 0, 0]));
      vl.insertSync('c', _v([0, 0, 1, 0]));
      expect(vl.train(), 0);
      expect(vl.rebuild(), 0);
      expect(vl.count(), 3);
      vl.close();
    });
  });

  // ---- SLSH ----

  group('SLSH', () {
    test('insert + count', () {
      final vl = VectorLayer.openSeparate(
          '${tempDir.path}/vltest', 'test', _slshFormat(), _rt);
      vl.insertSync('a', _v([1, 0, 0, 0]));
      vl.insertSync('b', _v([0, 1, 0, 0]));
      vl.insertSync('c', _v([0, 0, 1, 0]));
      expect(vl.count(), 3);
      vl.close();
    });

    test('search after train returns results', () {
      final vl = VectorLayer.openSeparate(
          '${tempDir.path}/vltest', 'test', _slshFormat(), _rt);
      vl.insertSync('a', _v([1, 0, 0, 0]));
      vl.insertSync('b', _v([0.9, 0.1, 0, 0]));
      vl.insertSync('c', _v([0, 1, 0, 0]));
      vl.insertSync('d', _v([0, 0, 1, 0]));
      expect(vl.train(), 0);

      final results = vl.searchSync(_v([1, 0, 0, 0]), 2);
      // SLSH is approximate; just verify we get results back.
      expect(results, isNotEmpty);
      vl.close();
    });

    test('train + rebuild preserve count', () {
      final vl = VectorLayer.openSeparate(
          '${tempDir.path}/vltest', 'test', _slshFormat(), _rt);
      vl.insertSync('a', _v([1, 0, 0, 0]));
      vl.insertSync('b', _v([0, 1, 0, 0]));
      vl.insertSync('c', _v([0, 0, 1, 0]));
      expect(vl.train(), 0);
      expect(vl.rebuild(), 0);
      expect(vl.count(), 3);
      vl.close();
    });

    test('delete reduces count', () {
      final vl = VectorLayer.openSeparate(
          '${tempDir.path}/vltest', 'test', _slshFormat(), _rt);
      vl.insertSync('a', _v([1, 0, 0, 0]));
      vl.insertSync('b', _v([0, 1, 0, 0]));
      expect(vl.count(), 2);
      vl.deleteSync('a');
      expect(vl.count(), 1);
      vl.close();
    });
  });

  // ---- error handling ----

  group('errors', () {
    test('create with dim=0 is rejected', () {
      expect(
        () => VectorLayer.openSeparate(
            '${tempDir.path}/vltest',
            'test',
            VectorFormat(indexType: IndexType.flat, dim: 0, distance: Distance.l2),
            _rt),
        throwsA(isA<WaveDBException>()),
      );
    });

    test('search on empty index returns empty list', () {
      final vl = VectorLayer.openSeparate(
          '${tempDir.path}/vltest', 'test', _flatFormat(), _rt);
      expect(vl.searchSync(_v([1, 0, 0, 0]), 5), isEmpty);
      vl.close();
    });
  });
}