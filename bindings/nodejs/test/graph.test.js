'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');
const { GraphLayer, g, Query, setDefaultGraph } = require('../lib/graph');
const { WaveDB } = require('../lib/wavedb');

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

  it('should support HasGt filter', function() {
    graph.insertSync('alice', 'age', '25');
    graph.insertSync('bob', 'age', '30');
    graph.insertSync('carol', 'age', '20');

    const result = g.HasGt('age', '25').All();
    assert.strictEqual(result.length, 1);
    assert.strictEqual(result[0], 'bob');
  });

  it('should support HasGte filter', function() {
    graph.insertSync('alice', 'age', '25');
    graph.insertSync('bob', 'age', '30');
    graph.insertSync('carol', 'age', '20');

    const result = g.HasGte('age', '25').All();
    assert.strictEqual(result.length, 2);
    assert.ok(result.includes('alice'));
    assert.ok(result.includes('bob'));
  });

  it('should support HasLt filter', function() {
    graph.insertSync('alice', 'age', '25');
    graph.insertSync('bob', 'age', '30');
    graph.insertSync('carol', 'age', '20');

    const result = g.HasLt('age', '25').All();
    assert.strictEqual(result.length, 1);
    assert.strictEqual(result[0], 'carol');
  });

  it('should support HasLte filter', function() {
    graph.insertSync('alice', 'age', '25');
    graph.insertSync('bob', 'age', '30');
    graph.insertSync('carol', 'age', '20');

    const result = g.HasLte('age', '25').All();
    assert.strictEqual(result.length, 2);
    assert.ok(result.includes('alice'));
    assert.ok(result.includes('carol'));
  });

  it('should chain HasGte with Out traversal', function() {
    graph.insertSync('alice', 'age', '25');
    graph.insertSync('alice', 'follows', 'carol');
    graph.insertSync('bob', 'age', '30');
    graph.insertSync('bob', 'follows', 'dave');
    graph.insertSync('carol', 'age', '20');

    const result = g.V('alice').HasGte('age', '25').Out('follows').All();
    assert.strictEqual(result.length, 1);
    assert.strictEqual(result[0], 'carol');
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
  // ── expandTriple: cross-subtree atomic batches ──

  describe('expandTriple', function() {
    // NOTE: do not name this `xgraph` — the outer describe's beforeEach/afterEach
    // owns that variable and closes it on teardown. Shadowing it leaks the
    // outer in-memory GraphLayer and corrupts process-exit cleanup.
    let xdb;
    let xgraph;
    let xtempDir;

    beforeEach(function() {
      xtempDir = fs.mkdtempSync('/tmp/wavedb-graph-expand-');
      xdb = new WaveDB(xtempDir);
      // Open a subtree at "xgraph" so the GraphLayer shares the xdb. expandTriple
      // emits keys in the ROOT xdb namespace (subtree prefix already prepended
      // by the C helper), so we feed the returned ops to xdb.batchSync (NOT
      // subtree.batchSync, which would re-prefix).
      const subtree = xdb.openSubtree('graph');
      xgraph = new GraphLayer(null, { subtree });
      subtree.close();
    });

    afterEach(function() {
      if (xgraph && !xgraph.isClosed) xgraph.close();
      if (xdb) xdb.close();
      if (xtempDir) fs.rmSync(xtempDir, { recursive: true, force: true });
    });

    it('returns one canonical root-namespace op per xgraph index', function() {
      const ops = xgraph.expandTriple('alice', 'knows', 'bob');
      assert.strictEqual(ops.length, 4);
      assert.ok(ops.every(op => op.type === 'put'));
      assert.ok(ops.every(op => op.value === ''));
      // Keys live in the ROOT xdb namespace: subtree prefix + delimiter + the
      // index key → "xgraph/spo/..." (single delimiter; the leading slash
      // from the xgraph key builder is stripped by the C helper). Pass these
      // to xdb.batchSync, NOT to a subtree batch (which would re-prefix).
      assert.ok(ops.every(op => op.key.startsWith('graph/')));
      const keys = ops.map(op => op.key);
      assert.ok(keys.includes('graph/spo/alice/knows/bob'));
    });

    it('is atomic-equivalent to insertSync when spliced into a root batch', function() {
      // One atomic batch: a content write in the root namespace plus a xgraph
      // triple expanded into root-namespace index ops.
      const ops = [
        { type: 'put', key: 'content/ep1/summary', value: 'alice met bob' },
      ].concat(xgraph.expandTriple('alice', 'knows', 'bob'));
      xdb.batchSync(ops);

      assert.strictEqual(xdb.getSync('content/ep1/summary').toString(),
                         'alice met bob');
      const result = xgraph.exec(g.V('alice').Out('knows').toString());
      assert.ok(result.includes('bob'));
    });

    it('delete=True yields del ops that remove the triple', function() {
      xgraph.insertSync('alice', 'knows', 'bob');
      assert.ok(xgraph.exec(g.V('alice').Out('knows').toString()).includes('bob'));

      xdb.batchSync(xgraph.expandTriple('alice', 'knows', 'bob', { delete: true }));
      assert.strictEqual(
        xgraph.exec(g.V('alice').Out('knows').toString()).includes('bob'),
        false);
    });

    it('stores the exact same index keys as insertSync', async function() {
      // Write via expandTriple+batchSync in one db, via insertSync in another.
      // The two write paths must be fully interchangeable — same on-disk keys.
      const dir1 = fs.mkdtempSync('/tmp/wavedb-graph-cmp-ins-');
      const db1 = new WaveDB(dir1);
      const st1 = db1.openSubtree('graph');
      const g1 = new GraphLayer(null, { subtree: st1 });
      g1.insertSync('alice', 'knows', 'bob');
      const insertKeys = new Set(await collectKeys(db1, 'graph/'));
      g1.close();
      st1.close();
      db1.close();
      fs.rmSync(dir1, { recursive: true, force: true });

      const dir2 = fs.mkdtempSync('/tmp/wavedb-graph-cmp-exp-');
      const db2 = new WaveDB(dir2);
      const st2 = db2.openSubtree('graph');
      const g2 = new GraphLayer(null, { subtree: st2 });
      db2.batchSync(g2.expandTriple('alice', 'knows', 'bob'));
      const expandKeys = new Set(await collectKeys(db2, 'graph/'));
      g2.close();
      st2.close();
      db2.close();
      fs.rmSync(dir2, { recursive: true, force: true });

      // Both write paths produce the same on-disk keys, and that set contains
      // the four expected SPO/POS/OSP/PSO index keys.
      assert.deepStrictEqual([...insertKeys].sort(), [...expandKeys].sort());
      const expected = new Set([
        'graph/spo/alice/knows/bob',
        'graph/pos/knows/bob/alice',
        'graph/osp/bob/alice/knows',
        'graph/pso/knows/alice/bob',
      ]);
      for (const k of expected) {
        assert.ok(insertKeys.has(k), `missing expected key: ${k}`);
      }
    });
  });

// Helper: collect all keys under `prefix` via a createReadStream.
// Returns a Promise that resolves to the array of keys once the stream ends.
function collectKeys(db, prefix) {
  return new Promise((resolve, reject) => {
    const stream = db.createReadStream({ start: prefix });
    const keys = [];
    stream.on('data', (entry) => {
      if (typeof entry.key === 'string') {
        keys.push(entry.key);
      } else if (Buffer.isBuffer(entry.key)) {
        keys.push(entry.key.toString('utf-8'));
      }
    });
    stream.on('end', () => resolve(keys));
    stream.on('error', reject);
  });
}