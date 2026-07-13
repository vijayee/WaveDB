'use strict';

// Tests for the VectorLayer Node.js binding.
//
// Mirrors the gtest coverage via mocha + the JS wrapper. Covers:
//   - Lifecycle (openSeparate, count, close)
//   - FLAT insert + search (with metadata)
//   - FLAT delete
//   - IVF insert + train + search
//   - SLSH insert + train + search
//   - reconfigure (runtime tier)
//   - Error handling (closed layer, wrong arg types)
//   - Async wrappers (insert/search returning Promises)
//
// Note: async wrappers currently call sync under the hood — the test
// just verifies the Promise shape, not actual non-blocking behaviour.

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { VectorLayer, IndexType, Distance } = require('../lib/vector_layer');
const { WaveDB } = require('../lib/wavedb');

describe('VectorLayer', function () {
  this.timeout(30000);

  let tmpDir;
  beforeEach(() => {
    tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'wavedb-vl-'));
  });
  afterEach(() => {
    try { fs.rmSync(tmpDir, { recursive: true, force: true }); } catch (_) {}
  });

  describe('lifecycle', () => {
    it('openSeparate + count (empty)', () => {
      const vl = VectorLayer.openSeparate(tmpDir, 'test', {
        index_type: IndexType.FLAT, dim: 4, distance: Distance.L2,
      }, { sync_only: 1 });
      assert.strictEqual(vl.count(), 0);
      assert.strictEqual(vl.isClosed, false);
      vl.close();
      assert.strictEqual(vl.isClosed, true);
    });

    it('close is idempotent', () => {
      const vl = VectorLayer.openSeparate(tmpDir, 'test', {
        index_type: IndexType.FLAT, dim: 4, distance: Distance.L2,
      }, { sync_only: 1 });
      vl.close();
      vl.close();  // no throw
    });

    it('throws on use after close', () => {
      const vl = VectorLayer.openSeparate(tmpDir, 'test', {
        index_type: IndexType.FLAT, dim: 4, distance: Distance.L2,
      }, { sync_only: 1 });
      vl.close();
      assert.throws(() => vl.count(), /closed/i);
    });

    it('rejects bad args to openSeparate', () => {
      assert.throws(() => VectorLayer.openSeparate('', 'test',
        { index_type: IndexType.FLAT, dim: 4 }, { sync_only: 1 }),
        /dbLocation/);
      assert.throws(() => VectorLayer.openSeparate(tmpDir, '',
        { index_type: IndexType.FLAT, dim: 4 }, { sync_only: 1 }),
        /indexName/);
    });
  });

  describe('FLAT', () => {
    let vl;
    beforeEach(() => {
      vl = VectorLayer.openSeparate(tmpDir, 'flat', {
        index_type: IndexType.FLAT, dim: 4, distance: Distance.L2,
      }, { sync_only: 1 });
    });
    afterEach(() => vl.close());

    it('insert + search returns nearest neighbors', () => {
      vl.insertSync('a', Float32Array.from([1, 2, 3, 4]));
      vl.insertSync('b', Float32Array.from([4, 3, 2, 1]));
      vl.insertSync('c', Float32Array.from([0, 0, 0, 0]));
      assert.strictEqual(vl.count(), 3);

      const r = vl.searchSync(Float32Array.from([1, 2, 3, 4]), 2);
      assert.strictEqual(r.length, 2);
      assert.strictEqual(r[0].id, 'a');
      assert.strictEqual(r[0].distance, 0);  // exact match -> L2 = 0
      assert.ok(typeof r[0].distance === 'number');
    });

    it('search with k > count returns all', () => {
      vl.insertSync('a', Float32Array.from([1, 2, 3, 4]));
      vl.insertSync('b', Float32Array.from([2, 3, 4, 5]));
      const r = vl.searchSync(Float32Array.from([1, 2, 3, 4]), 10);
      assert.strictEqual(r.length, 2);
    });

    it('preserves metadata (Buffer)', () => {
      const meta = Buffer.from('hello-meta');
      vl.insertSync('a', Float32Array.from([1, 2, 3, 4]), meta);
      const r = vl.searchSync(Float32Array.from([1, 2, 3, 4]), 1);
      assert.strictEqual(r.length, 1);
      assert.strictEqual(r[0].id, 'a');
      assert.ok(r[0].metadata !== null && r[0].metadata !== undefined);
      assert.ok(Buffer.isBuffer(r[0].metadata));
      assert.strictEqual(r[0].metadata.toString(), 'hello-meta');
    });

    it('null metadata returns null in result', () => {
      vl.insertSync('a', Float32Array.from([1, 2, 3, 4]), null);
      const r = vl.searchSync(Float32Array.from([1, 2, 3, 4]), 1);
      assert.strictEqual(r.length, 1);
      assert.strictEqual(r[0].metadata, null);
    });

    it('delete removes a vector', () => {
      vl.insertSync('a', Float32Array.from([1, 2, 3, 4]));
      vl.insertSync('b', Float32Array.from([4, 3, 2, 1]));
      assert.strictEqual(vl.count(), 2);
      vl.deleteSync('a');
      assert.strictEqual(vl.count(), 1);
      const r = vl.searchSync(Float32Array.from([1, 2, 3, 4]), 2);
      assert.strictEqual(r.length, 1);
      assert.strictEqual(r[0].id, 'b');
    });

    it('train + rebuild are no-ops for FLAT', () => {
      vl.insertSync('a', Float32Array.from([1, 2, 3, 4]));
      // Should not throw.
      vl.train();
      vl.rebuild();
      assert.strictEqual(vl.count(), 1);
    });

    it('throws on non-Float32Array vec', () => {
      assert.throws(() => vl.insertSync('a', [1, 2, 3, 4]), /Float32Array/);
    });

    it('throws on non-Buffer metadata', () => {
      assert.throws(
        () => vl.insertSync('a', Float32Array.from([1, 2, 3, 4]), 'not-a-buffer'),
        /Buffer/
      );
    });
  });

  describe('IVF', () => {
    let vl;
    beforeEach(() => {
      vl = VectorLayer.openSeparate(tmpDir, 'ivf', {
        index_type: IndexType.IVF, dim: 8, distance: Distance.COSINE,
        ivf_n_clusters: 4,
      }, { sync_only: 1, ivf_nprobe: 2, ivf_flat_until: 0 });
    });
    afterEach(() => vl.close());

    it('insert + train + search', () => {
      // Insert enough vectors to make IVF meaningful (>= n_clusters).
      for (let i = 0; i < 16; i++) {
        const v = new Float32Array(8);
        for (let j = 0; j < 8; j++) v[j] = (i + j) * 0.01;
        vl.insertSync(`vec-${i}`, v);
      }
      assert.strictEqual(vl.count(), 16);

      // Train should compute centroids.
      vl.train();

      // Search must return some neighbors.
      const q = new Float32Array(8);
      for (let j = 0; j < 8; j++) q[j] = j * 0.01;
      const r = vl.searchSync(q, 5);
      assert.ok(r.length > 0);
      assert.ok(r.length <= 5);
      // Results should have id + distance + metadata fields.
      assert.ok(typeof r[0].id === 'string');
      assert.ok(typeof r[0].distance === 'number');
    });

    it('rebuild rewrites memberships', () => {
      for (let i = 0; i < 16; i++) {
        const v = new Float32Array(8);
        for (let j = 0; j < 8; j++) v[j] = (i + j) * 0.01;
        vl.insertSync(`vec-${i}`, v);
      }
      vl.train();
      vl.rebuild();  // should not throw
      assert.strictEqual(vl.count(), 16);
    });
  });

  describe('SLSH', () => {
    let vl;
    beforeEach(() => {
      vl = VectorLayer.openSeparate(tmpDir, 'slsh', {
        index_type: IndexType.SLSH, dim: 16, distance: Distance.L2,
        slsh_lsh_tables: 4, slsh_hash_bits: 12, slsh_bucket_width: 2.0,
      }, { sync_only: 1, slsh_scan_radius: 100 });
    });
    afterEach(() => vl.close());

    it('insert + train + search', () => {
      // SLSH needs a reasonable corpus for the bidirectional scan to
      // find neighbors. 32 vectors of dim 16.
      for (let i = 0; i < 32; i++) {
        const v = new Float32Array(16);
        for (let j = 0; j < 16; j++) v[j] = Math.sin(i * 0.1 + j) * 10;
        vl.insertSync(`s-${i}`, v);
      }
      assert.strictEqual(vl.count(), 32);

      vl.train();

      const q = new Float32Array(16);
      for (let j = 0; j < 16; j++) q[j] = Math.sin(0.1 + j) * 10;
      const r = vl.searchSync(q, 5);
      assert.ok(r.length > 0, 'SLSH search should return results');
      assert.ok(r.length <= 5);
    });
  });

  describe('reconfigure', () => {
    it('updates runtime params', () => {
      const vl = VectorLayer.openSeparate(tmpDir, 'rc', {
        index_type: IndexType.IVF, dim: 4, distance: Distance.L2,
      }, { sync_only: 1, ivf_nprobe: 4 });
      // Should not throw.
      vl.reconfigure({ ivf_nprobe: 8, slsh_scan_radius: 50 });
      vl.close();
    });
  });

  describe('async wrappers', () => {
    it('insert + search return Promises', async () => {
      const vl = VectorLayer.openSeparate(tmpDir, 'async', {
        index_type: IndexType.FLAT, dim: 4, distance: Distance.L2,
      }, { sync_only: 1 });
      try {
        await vl.insert('a', Float32Array.from([1, 2, 3, 4]));
        await vl.insert('b', Float32Array.from([4, 3, 2, 1]));
        const r = await vl.search(Float32Array.from([1, 2, 3, 4]), 2);
        assert.ok(Array.isArray(r));
        assert.strictEqual(r.length, 2);
        assert.strictEqual(r[0].id, 'a');
        const del = await vl.del('a');
        assert.strictEqual(del, undefined);
        assert.strictEqual(vl.count(), 1);
      } finally {
        vl.close();
      }
    });
  });

  describe('embedded mode (VectorLayer.open on a WaveDB instance)', () => {
    it('shares the database with a WaveDB instance', () => {
      const dbPath = path.join(tmpDir, 'embedded');
      const db = new WaveDB(dbPath);
      try {
        const vl = VectorLayer.open('vec-idx', db, {
          index_type: IndexType.FLAT, dim: 4, distance: Distance.L2,
        }, { sync_only: 1 });
        try {
          vl.insertSync('a', Float32Array.from([1, 2, 3, 4]));
          vl.insertSync('b', Float32Array.from([4, 3, 2, 1]));
          assert.strictEqual(vl.count(), 2);
          const r = vl.searchSync(Float32Array.from([1, 2, 3, 4]), 1);
          assert.strictEqual(r.length, 1);
          assert.strictEqual(r[0].id, 'a');
        } finally {
          vl.close();
        }
      } finally {
        db.close();
      }
    });
  });
});