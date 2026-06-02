'use strict';

const assert = require('assert');
const { GraphLayer, g, Query, setDefaultGraph } = require('../lib/graph');

describe('GraphLayer', function() {
  this.timeout(10000);

  let graph;
  beforeEach(function() {
    graph = new GraphLayer();
  });
  afterEach(function() {
    graph.close();
  });

  it('should insert and query triples', function() {
    graph.insertSync('clip_abc', 'tagged_with', 'gaming');
    graph.insertSync('clip_abc', 'tagged_with', 'tutorial');
    graph.insertSync('clip_abc', 'created_by', 'alice');

    const result = g.V('clip_abc').Out('tagged_with').All();
    assert.ok(Array.isArray(result));
    assert.strictEqual(result.length, 2);
    assert.ok(result.includes('gaming'));
    assert.ok(result.includes('tutorial'));
  });

  it('should traverse incoming edges', function() {
    graph.insertSync('clip_abc', 'tagged_with', 'gaming');
    graph.insertSync('clip_xyz', 'tagged_with', 'gaming');

    const result = g.V('gaming').In('tagged_with').All();
    assert.strictEqual(result.length, 2);
    assert.ok(result.includes('clip_abc'));
    assert.ok(result.includes('clip_xyz'));
  });

  it('should do multi-hop traversals', function() {
    graph.insertSync('alice', 'follows', 'bob');
    graph.insertSync('bob', 'likes', 'clip_abc');

    const result = g.V('alice').Out('follows').Out('likes').All();
    assert.strictEqual(result.length, 1);
    assert.strictEqual(result[0], 'clip_abc');
  });

  it('should handle intersection queries', function() {
    graph.insertSync('clip_abc', 'tagged_with', 'gaming');
    graph.insertSync('clip_abc', 'tagged_with', 'tutorial');
    graph.insertSync('clip_xyz', 'tagged_with', 'gaming');

    const result = g.V('gaming').In('tagged_with')
      .And(g.V('tutorial').In('tagged_with'))
      .All();

    assert.strictEqual(result.length, 1);
    assert.strictEqual(result[0], 'clip_abc');
  });

  it('should handle union queries (Or)', function() {
    graph.insertSync('clip_abc', 'tagged_with', 'gaming');
    graph.insertSync('clip_xyz', 'tagged_with', 'tutorial');

    const result = g.V('clip_abc').Out('tagged_with')
      .Or(g.V('clip_xyz').Out('tagged_with'))
      .All();

    assert.strictEqual(result.length, 2);
    assert.ok(result.includes('gaming'));
    assert.ok(result.includes('tutorial'));
  });

  it('should handle Has filters', function() {
    graph.insertSync('clip_abc', 'name', 'My Clip');
    graph.insertSync('clip_xyz', 'name', 'Other Clip');

    const result = g.Has('name', 'My Clip').All();
    assert.strictEqual(result.length, 1);
    assert.strictEqual(result[0], 'clip_abc');
  });

  it('should support Limit', function() {
    graph.insertSync('clip_abc', 'tagged_with', 'gaming');
    graph.insertSync('clip_xyz', 'tagged_with', 'gaming');

    const result = g.V('gaming').In('tagged_with').Limit(1).All();
    assert.strictEqual(result.length, 1);
  });

  it('should support Count', function() {
    graph.insertSync('clip_abc', 'tagged_with', 'gaming');
    graph.insertSync('clip_xyz', 'tagged_with', 'gaming');

    const count = graph.count('g.V("gaming").In("tagged_with")');
    assert.strictEqual(count, 2);
  });

  it('should delete triples', function() {
    graph.insertSync('clip_abc', 'tagged_with', 'gaming');
    graph.deleteSync('clip_abc', 'tagged_with', 'gaming');

    const result = g.V('clip_abc').Out('tagged_with').All();
    assert.strictEqual(result.length, 0);
  });

  it('should handle empty results', function() {
    const result = g.V('nonexistent').Out('unknown').All();
    assert.ok(Array.isArray(result));
    assert.strictEqual(result.length, 0);
  });

  it('should accept DSL strings directly via exec', function() {
    graph.insertSync('clip_abc', 'tagged_with', 'gaming');

    const result = graph.exec('g.V("clip_abc").Out("tagged_with")');
    assert.strictEqual(result.length, 1);
    assert.strictEqual(result[0], 'gaming');
  });

  it('should accept Query objects via exec', function() {
    graph.insertSync('clip_abc', 'tagged_with', 'gaming');

    const q = new Query(graph);
    q.V('clip_abc').Out('tagged_with');
    const result = graph.exec(q);
    assert.strictEqual(result.length, 1);
  });

  it('should be able to use Query.toString() for debugging', function() {
    const q = g.V('alice').Out('follows').Out('likes').Limit(5);
    assert.strictEqual(q.toString(), 'g.V("alice").Out("follows").Out("likes").Limit(5)');
  });

  it('should handle difference queries', function() {
    graph.insertSync('clip_abc', 'tagged_with', 'gaming');
    graph.insertSync('clip_abc', 'tagged_with', 'tutorial');
    graph.insertSync('clip_xyz', 'tagged_with', 'gaming');

    const result = g.V('gaming').In('tagged_with')
      .Not(g.V('tutorial').In('tagged_with'))
      .All();

    assert.strictEqual(result.length, 1);
    assert.strictEqual(result[0], 'clip_xyz');
  });

  it('should support morphism definition and Follow', function() {
    graph.insertSync('alice', 'follows', 'bob');
    graph.insertSync('bob', 'likes', 'clip_abc');

    graph.defineMorphism('friends_content',
      'g.Morphism("friends_content").Out("follows").Out("likes")');

    const result = g.V('alice').Follow('friends_content').All();
    assert.strictEqual(result.length, 1);
    assert.strictEqual(result[0], 'clip_abc');
  });

  it('should support parseSchema', function() {
    graph.parseSchema('type Clip @index(spo, pos) { tagged_with: [Tag]; name: String @index(pos); }');
    // Schema is stored; insert should still work
    graph.insertSync('clip_abc', 'tagged_with', 'gaming');
    const result = g.V('clip_abc').Out('tagged_with').All();
    assert.strictEqual(result.length, 1);
    assert.strictEqual(result[0], 'gaming');
  });

  // Async operations

  describe('async operations', function() {
    it('should insert triples asynchronously', async function() {
      await graph.insert('clip_abc', 'tagged_with', 'gaming');
      await graph.insert('clip_abc', 'tagged_with', 'tutorial');

      const result = g.V('clip_abc').Out('tagged_with').All();
      assert.strictEqual(result.length, 2);
      assert.ok(result.includes('gaming'));
      assert.ok(result.includes('tutorial'));
    });

    it('should delete triples asynchronously', async function() {
      graph.insertSync('clip_abc', 'tagged_with', 'gaming');
      await graph.del('clip_abc', 'tagged_with', 'gaming');

      const result = g.V('clip_abc').Out('tagged_with').All();
      assert.strictEqual(result.length, 0);
    });

    it('should query asynchronously', async function() {
      graph.insertSync('clip_abc', 'tagged_with', 'gaming');
      graph.insertSync('clip_xyz', 'tagged_with', 'gaming');

      const result = await graph.query('g.V("gaming").In("tagged_with")');
      assert.ok(Array.isArray(result));
      assert.strictEqual(result.length, 2);
      assert.ok(result.includes('clip_abc'));
      assert.ok(result.includes('clip_xyz'));
    });

    it('should query asynchronously with Query object', async function() {
      graph.insertSync('alice', 'follows', 'bob');
      graph.insertSync('bob', 'likes', 'clip_abc');

      const q = new Query(graph);
      q.V('alice').Out('follows').Out('likes');
      const result = await graph.query(q);
      assert.strictEqual(result.length, 1);
      assert.strictEqual(result[0], 'clip_abc');
    });

    it('should reject on query parse error', async function() {
      try {
        await graph.query('g.InvalidStep("test")');
        assert.fail('Should have thrown');
      } catch (e) {
        assert.ok(e.message.length > 0);
      }
    });

    it('should handle multiple async inserts concurrently', async function() {
      const promises = [
        graph.insert('a', 'knows', 'b'),
        graph.insert('b', 'knows', 'c'),
        graph.insert('c', 'knows', 'd'),
      ];
      await Promise.all(promises);

      const result = g.V('a').Out('knows').Out('knows').All();
      assert.strictEqual(result.length, 1);
      assert.strictEqual(result[0], 'c');
    });
  });
});