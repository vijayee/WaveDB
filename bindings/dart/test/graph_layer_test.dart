import 'package:test/test.dart';
import 'package:wavedb/wavedb.dart';

void main() {
  late GraphLayer graph;

  setUp(() {
    graph = GraphLayer();
  });

  tearDown(() {
    graph.close();
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

  test('GraphLayerException on failed insert', () {
    graph.close();
    expect(
      () => graph.insertSync('a', 'b', 'c'),
      throwsA(isA<StateError>()),
    );
  });
}
