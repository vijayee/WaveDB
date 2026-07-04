import 'dart:io';
import 'package:test/test.dart';
import 'package:wavedb/wavedb.dart';

void main() {
  late GraphLayer graph;

  setUp(() {
    graph = GraphLayer();
  });

  tearDown(() async {
    await graph.close();
  });

  test('insert and query triples', () {
    graph.insertSync('clip_abc', 'tagged_with', 'gaming');
    graph.insertSync('clip_abc', 'tagged_with', 'tutorial');
    graph.insertSync('clip_abc', 'created_by', 'alice');

    final results = g().V('clip_abc').Out('tagged_with').All();
    expect(results, hasLength(2));
    expect(results, contains('gaming'));
    expect(results, contains('tutorial'));
  });

  test('traverse incoming edges', () {
    graph.insertSync('clip_abc', 'tagged_with', 'gaming');
    graph.insertSync('clip_xyz', 'tagged_with', 'gaming');

    final results = g().V('gaming').In('tagged_with').All();
    expect(results, hasLength(2));
    expect(results, contains('clip_abc'));
    expect(results, contains('clip_xyz'));
  });

  test('multi-hop traversal', () {
    graph.insertSync('alice', 'follows', 'bob');
    graph.insertSync('bob', 'likes', 'clip_abc');

    final results = g().V('alice').Out('follows').Out('likes').All();
    expect(results, hasLength(1));
    expect(results[0], 'clip_abc');
  });

  test('intersection query', () {
    graph.insertSync('clip_abc', 'tagged_with', 'gaming');
    graph.insertSync('clip_abc', 'tagged_with', 'tutorial');
    graph.insertSync('clip_xyz', 'tagged_with', 'gaming');

    final results = g().V('gaming').In('tagged_with')
        .And(g().V('tutorial').In('tagged_with'))
        .All();

    expect(results, hasLength(1));
    expect(results[0], 'clip_abc');
  });

  test('has filter', () {
    graph.insertSync('clip_abc', 'name', 'My Clip');
    graph.insertSync('clip_xyz', 'name', 'Other Clip');

    final results = g().Has('name', 'My Clip').All();
    expect(results, hasLength(1));
    expect(results[0], 'clip_abc');
  });

  test('limit', () {
    graph.insertSync('clip_abc', 'tagged_with', 'gaming');
    graph.insertSync('clip_xyz', 'tagged_with', 'gaming');

    final results = g().V('gaming').In('tagged_with').Limit(1).All();
    expect(results, hasLength(1));
  });

  test('count', () {
    graph.insertSync('clip_abc', 'tagged_with', 'gaming');
    graph.insertSync('clip_xyz', 'tagged_with', 'gaming');

    final count = graph.count('g.V("gaming").In("tagged_with")');
    expect(count, 2);
  });

  test('delete triple', () {
    graph.insertSync('clip_abc', 'tagged_with', 'gaming');
    graph.deleteSync('clip_abc', 'tagged_with', 'gaming');

    final results = g().V('clip_abc').Out('tagged_with').All();
    expect(results, hasLength(0));
  });

  test('empty result', () {
    final results = g().V('nonexistent').Out('unknown').All();
    expect(results, hasLength(0));
  });

  test('DSL string via exec', () {
    graph.insertSync('clip_abc', 'tagged_with', 'gaming');

    final results = graph.exec('g.V("clip_abc").Out("tagged_with")');
    expect(results, hasLength(1));
    expect(results[0], 'gaming');
  });

  test('Query.toString() debugging', () {
    final q = g().V('alice').Out('follows').Out('likes').Limit(5);
    expect(q.toString(), 'g.V("alice").Out("follows").Out("likes").Limit(5)');
  });

  test('GraphLayerException on failed insert', () async {
    await graph.close();
    expect(
      () => graph.insertSync('a', 'b', 'c'),
      throwsA(isA<StateError>()),
    );
  });

  test('difference query', () {
    graph.insertSync('clip_abc', 'tagged_with', 'gaming');
    graph.insertSync('clip_abc', 'tagged_with', 'tutorial');
    graph.insertSync('clip_xyz', 'tagged_with', 'gaming');

    final results = g()
        .V('gaming').In('tagged_with')
        .Not(g().V('tutorial').In('tagged_with'))
        .All();
    expect(results, hasLength(1));
    expect(results[0], 'clip_xyz');
  });

  test('morphism definition and Follow', () {
    graph.insertSync('alice', 'follows', 'bob');
    graph.insertSync('bob', 'likes', 'clip_abc');

    graph.defineMorphism('friends_content',
        'g.Morphism("friends_content").Out("follows").Out("likes")');

    final results = g().V('alice').Follow('friends_content').All();
    expect(results, hasLength(1));
    expect(results[0], 'clip_abc');
  });

  test('parseSchema', () {
    graph.parseSchema(
        'type Clip @index(spo, pos) { tagged_with: [Tag]; name: String @index(pos); }');
    graph.insertSync('clip_abc', 'tagged_with', 'gaming');
    final results = g().V('clip_abc').Out('tagged_with').All();
    expect(results, hasLength(1));
    expect(results[0], 'gaming');
  });

  // Range predicate tests are skipped until Dart FFI encoding issue is resolved.
  // The DSL 'Has("age", >, "25")' fails parsing from Dart but works from C and Node.js.
  // The HasGt/HasGte/HasLt/HasLte methods generate correct DSL strings.
  test('HasGt filter', () {
    graph.insertSync('alice', 'age', '25');
    graph.insertSync('bob', 'age', '30');
    graph.insertSync('carol', 'age', '20');

    final results = g().HasGt('age', '25').All();
    expect(results, hasLength(1));
    expect(results[0], 'bob');
  });

  test('HasGte filter', () {
    graph.insertSync('alice', 'age', '25');
    graph.insertSync('bob', 'age', '30');
    graph.insertSync('carol', 'age', '20');

    final results = g().HasGte('age', '25').All();
    expect(results, hasLength(2));
    expect(results, contains('alice'));
    expect(results, contains('bob'));
  });

  test('HasLt filter', () {
    graph.insertSync('alice', 'age', '25');
    graph.insertSync('bob', 'age', '30');
    graph.insertSync('carol', 'age', '20');

    final results = g().HasLt('age', '25').All();
    expect(results, hasLength(1));
    expect(results[0], 'carol');
  });

  test('HasLte filter', () {
    graph.insertSync('alice', 'age', '25');
    graph.insertSync('bob', 'age', '30');
    graph.insertSync('carol', 'age', '20');

    final results = g().HasLte('age', '25').All();
    expect(results, hasLength(2));
    expect(results, contains('alice'));
    expect(results, contains('carol'));
  });

  test('HasGte chained with Out traversal', () {
    graph.insertSync('alice', 'age', '25');
    graph.insertSync('alice', 'follows', 'carol');
    graph.insertSync('bob', 'age', '30');
    graph.insertSync('bob', 'follows', 'dave');
    graph.insertSync('carol', 'age', '20');

    final results = g().V('alice').HasGte('age', '25').Out('follows').All();
    expect(results, hasLength(1));
    expect(results[0], 'carol');
  });

  test('union query (Or)', () {
    graph.insertSync('clip_abc', 'tagged_with', 'gaming');
    graph.insertSync('clip_xyz', 'tagged_with', 'tutorial');

    final results = g()
        .V('clip_abc').Out('tagged_with')
        .Or(g().V('clip_xyz').Out('tagged_with'))
        .All();
    expect(results, hasLength(2));
    expect(results, contains('gaming'));
    expect(results, contains('tutorial'));
  });

  test('query with Query object via exec', () {
    graph.insertSync('clip_abc', 'tagged_with', 'gaming');

    final q = GraphQuery(graph)..V('clip_abc').Out('tagged_with');
    final results = graph.exec(q);
    expect(results, hasLength(1));
    expect(results[0], 'gaming');
  });

  group('async operations', () {
    test('async insert and query', () async {
      await graph.insert('clip_abc', 'tagged_with', 'gaming');
      await graph.insert('clip_abc', 'tagged_with', 'tutorial');

      final results = g().V('clip_abc').Out('tagged_with').All();
      expect(results, hasLength(2));
      expect(results, contains('gaming'));
      expect(results, contains('tutorial'));
    });

    test('async delete', () async {
      graph.insertSync('clip_abc', 'tagged_with', 'gaming');
      await graph.del('clip_abc', 'tagged_with', 'gaming');

      final results = g().V('clip_abc').Out('tagged_with').All();
      expect(results, hasLength(0));
    });

    test('concurrent async inserts', () async {
      await Future.wait([
        graph.insert('a', 'knows', 'b'),
        graph.insert('b', 'knows', 'c'),
        graph.insert('c', 'knows', 'd'),
      ]);

      final results = g().V('a').Out('knows').Out('knows').All();
      expect(results, hasLength(1));
      expect(results[0], 'c');
    });
  });

  // ── expandTriple: cross-subtree atomic batches ──

  group('expandTriple', () {
    late Directory tempDir;
    late WaveDB db;
    late GraphLayer graph;

    setUp(() async {
      tempDir = await Directory.systemTemp.createTemp('wavedb_graph_expand_');
      db = WaveDB(tempDir.path);
      // Open a subtree at "graph" so the GraphLayer shares the db. expandTriple
      // emits keys in the ROOT db namespace (subtree prefix already prepended
      // by the C helper), so we feed the returned ops to db.batchSync (NOT
      // subtree.batchSync, which would re-prefix).
      final subtree = db.openSubtree('graph');
      graph = GraphLayer(null, subtree);
      subtree.close();
    });

    tearDown(() async {
      await graph.close();
      db.close();
      try {
        await tempDir.delete(recursive: true);
      } catch (_) {}
    });

    test('returns one canonical root-namespace op per graph index', () {
      final ops = graph.expandTriple('alice', 'knows', 'bob');
      expect(ops, hasLength(4));
      expect(ops.every((op) => op['type'] == 'put'), isTrue);
      expect(ops.every((op) => op['value'] == ''), isTrue);
      // Keys live in the ROOT db namespace: subtree prefix + delimiter + the
      // index key → "graph/spo/..." (single delimiter; the leading slash from
      // the graph key builder is stripped by the C helper). Pass these to
      // db.batchSync, NOT to a subtree batch (which would re-prefix).
      expect(ops.every((op) => (op['key'] as String).startsWith('graph/')),
             isTrue);
      final keys = ops.map((op) => op['key'] as String).toSet();
      expect(keys, contains('graph/spo/alice/knows/bob'));
    });

    test('is atomic-equivalent to insertSync when spliced into a root batch', () {
      // One atomic batch: a content write in the root namespace plus a graph
      // triple expanded into root-namespace index ops.
      final ops = [
        {'type': 'put', 'key': 'content/ep1/summary', 'value': 'alice met bob'},
        ...graph.expandTriple('alice', 'knows', 'bob'),
      ];
      db.batchSync(ops);

      expect(db.getSync('content/ep1/summary'), 'alice met bob');
      final result = graph.exec(g().V('alice').Out('knows'));
      expect(result, contains('bob'));
    });

    test('delete=true yields del ops that remove the triple', () {
      graph.insertSync('alice', 'knows', 'bob');
      expect(graph.exec(g().V('alice').Out('knows')), contains('bob'));

      db.batchSync(graph.expandTriple('alice', 'knows', 'bob', delete: true));
      expect(graph.exec(g().V('alice').Out('knows')), isNot(contains('bob')));
    });

    test('stores the exact same index keys as insertSync', () async {
      // Write via expandTriple+batchSync in one db, via insertSync in another.
      // The two write paths must be fully interchangeable — same on-disk keys.
      final expected = {
        'graph/spo/alice/knows/bob',
        'graph/pos/knows/bob/alice',
        'graph/osp/bob/alice/knows',
        'graph/pso/knows/alice/bob',
      };

      final dir1 = await Directory.systemTemp.createTemp('wavedb_graph_cmp_ins_');
      final db1 = WaveDB(dir1.path);
      final st1 = db1.openSubtree('graph');
      final g1 = GraphLayer(null, st1);
      g1.insertSync('alice', 'knows', 'bob');
      final insertKeys = await collectKeys(db1, 'graph/');
      await g1.close();
      st1.close();
      db1.close();
      await dir1.delete(recursive: true);

      final dir2 = await Directory.systemTemp.createTemp('wavedb_graph_cmp_exp_');
      final db2 = WaveDB(dir2.path);
      final st2 = db2.openSubtree('graph');
      final g2 = GraphLayer(null, st2);
      db2.batchSync(g2.expandTriple('alice', 'knows', 'bob'));
      final expandKeys = await collectKeys(db2, 'graph/');
      await g2.close();
      st2.close();
      db2.close();
      await dir2.delete(recursive: true);

      // Both write paths produce the same on-disk keys, and that set contains
      // the four expected SPO/POS/OSP/PSO index keys.
      expect(insertKeys.toSet(), equals(expandKeys.toSet()));
      for (final k in expected) {
        expect(insertKeys, contains(k));
      }
    });
  });
}

// Helper: collect all keys under `prefix` via createReadStream.
Future<List<String>> collectKeys(WaveDB db, String prefix) async {
  final keys = <String>[];
  await for (final entry in db.createReadStream(start: prefix)) {
    final k = entry.key;
    if (k is String) {
      keys.add(k);
    } else if (k is List<int>) {
      keys.add(String.fromCharCodes(k));
    }
  }
  return keys;
}
