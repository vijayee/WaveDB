const assert = require('assert');
const { WaveDB } = require('../lib/wavedb.js');
const fs = require('fs');
const path = require('path');

describe('WaveDB Integration', () => {
  let db;
  let testDbPath;

  beforeEach(() => {
    testDbPath = `/tmp/wavedb-integration-${Date.now()}-${Math.random().toString(36).substr(2, 9)}`;
    db = new WaveDB(testDbPath);
  });

  afterEach(() => {
    if (db) {
      db.close();
    }
    if (fs.existsSync(testDbPath)) {
      fs.rmSync(testDbPath, { recursive: true });
    }
  });

  describe('concurrent operations', () => {
    it('should handle concurrent writes', async () => {
      const writes = [];
      for (let i = 0; i < 100; i++) {
        writes.push(db.put(`key${i}`, `value${i}`));
      }

      await Promise.all(writes);

      for (let i = 0; i < 100; i++) {
        const value = await db.get(`key${i}`);
        assert.strictEqual(value, `value${i}`);
      }
    });

    it('should handle concurrent reads', async () => {
      for (let i = 0; i < 50; i++) {
        await db.put(`key${i}`, `value${i}`);
      }

      const reads = [];
      for (let i = 0; i < 50; i++) {
        reads.push(db.get(`key${i}`));
      }

      const values = await Promise.all(reads);
      for (let i = 0; i < 50; i++) {
        assert.strictEqual(values[i], `value${i}`);
      }
    });

    it('should handle mixed concurrent operations', async () => {
      // First, set up data for delete test sequentially
      for (let i = 30; i < 40; i++) {
        await db.put(`delkey${i}`, `value${i}`);
      }

      // Now do concurrent writes and reads
      const operations = [];

      // Writes
      for (let i = 0; i < 30; i++) {
        operations.push(db.put(`key${i}`, `value${i}`));
      }

      // Reads (some may return null initially)
      for (let i = 0; i < 20; i++) {
        operations.push(db.get(`key${i}`));
      }

      // Concurrent deletes (data already set up above)
      for (let i = 30; i < 40; i++) {
        operations.push(db.del(`delkey${i}`));
      }

      await Promise.all(operations);

      // Verify writes
      for (let i = 0; i < 30; i++) {
        const value = await db.get(`key${i}`);
        assert.strictEqual(value, `value${i}`);
      }

      // Verify deletes
      for (let i = 30; i < 40; i++) {
        const value = await db.get(`delkey${i}`);
        assert.strictEqual(value, null);
      }
    });
  });

  describe('large values', () => {
    it('should handle large string values', async () => {
      const large = 'x'.repeat(1024 * 1024);  // 1MB
      await db.put('large/key', large);
      const value = await db.get('large/key');
      assert.strictEqual(value.length, large.length);
    });

    it('should handle large binary values', async () => {
      const large = Buffer.alloc(1024 * 1024, 0xAB);
      await db.put('large/binary', large);
      const value = await db.get('large/binary');
      assert.deepStrictEqual(value, large);
    });
  });

  describe('persistence', () => {
    it('should persist data across database restarts', async () => {
      db.putSync('persistent/key', 'value');
      db.putSync('persistent/another', 'value2');

      db.close();

      // Reopen database
      db = new WaveDB(testDbPath);

      const value1 = db.getSync('persistent/key');
      const value2 = db.getSync('persistent/another');

      assert.strictEqual(value1, 'value');
      assert.strictEqual(value2, 'value2');
    });
  });

  describe('deep paths', () => {
    it('should handle deeply nested paths', async () => {
      const deepPath = ['level1', 'level2', 'level3', 'level4', 'level5', 'key'];
      await db.put(deepPath, 'deep-value');
      const value = await db.get(deepPath);
      assert.strictEqual(value, 'deep-value');
    });

    it('should handle path with many segments', async () => {
      const path = [];
      for (let i = 0; i < 100; i++) {
        path.push(`seg${i}`);
      }
      await db.put(path, 'value');
      const value = await db.get(path);
      assert.strictEqual(value, 'value');
    });
  });

  describe('batch operations', () => {
    it('should handle large batches', async () => {
      const ops = [];
      for (let i = 0; i < 1000; i++) {
        ops.push({ type: 'put', key: `batch/key${i}`, value: `value${i}` });
      }

      await db.batch(ops);

      for (let i = 0; i < 1000; i++) {
        const value = await db.get(`batch/key${i}`);
        assert.strictEqual(value, `value${i}`);
      }
    });

    it('should handle batches with mixed operations', async () => {
      // Initial data
      await db.put('initial/key1', 'value1');
      await db.put('initial/key2', 'value2');

      // Batch with puts and deletes
      await db.batch([
        { type: 'put', key: 'initial/key3', value: 'value3' },
        { type: 'del', key: 'initial/key1' },
        { type: 'put', key: 'initial/key2', value: 'updated2' }
      ]);

      assert.strictEqual(await db.get('initial/key1'), null);
      assert.strictEqual(await db.get('initial/key2'), 'updated2');
      assert.strictEqual(await db.get('initial/key3'), 'value3');
    });
  });
});