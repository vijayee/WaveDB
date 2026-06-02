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
}
