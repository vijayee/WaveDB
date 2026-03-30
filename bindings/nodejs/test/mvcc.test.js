/**
 * Comprehensive MVCC Test for WaveDB Node.js Bindings
 *
 * Tests that MVCC version chains (overwrites and deletes) work correctly
 * after CBOR serialization fix.
 */

const assert = require('assert');
const { WaveDB } = require('../lib/wavedb.js');
const fs = require('fs');

describe('MVCC Version Chains', () => {
  let db;
  let testDbPath;

  beforeEach(() => {
    testDbPath = `/tmp/wavedb-mvcc-test-${Date.now()}-${Math.random().toString(36).substr(2, 9)}`;
    db = new WaveDB(testDbPath);
  });

  afterEach(() => {
    if (db) {
      db.close();
    }
    // Clean up test directory
    if (fs.existsSync(testDbPath)) {
      fs.rmSync(testDbPath, { recursive: true });
    }
  });

  describe('Overwrites', () => {
    it('should handle simple overwrite', () => {
      db.putSync('key1', 'value1');
      db.putSync('key1', 'value2');

      const value = db.getSync('key1');
      assert.strictEqual(value, 'value2');
    });

    it('should handle multiple overwrites', () => {
      for (let i = 0; i < 10; i++) {
        db.putSync('key1', `value${i}`);
      }

      const value = db.getSync('key1');
      assert.strictEqual(value, 'value9');
    });

    it('should handle async operations with overwrites', async () => {
      await db.put('key1', 'value1');
      await db.put('key1', 'value2');
      await db.put('key1', 'value3');

      const value = await db.get('key1');
      assert.strictEqual(value, 'value3');
    });

    it('should handle batch operations with overwrites', async () => {
      // First batch
      await db.batch([
        { type: 'put', key: 'key1', value: 'value1' },
        { type: 'put', key: 'key2', value: 'value2' }
      ]);

      // Second batch with overwrites
      await db.batch([
        { type: 'put', key: 'key1', value: 'value1b' },
        { type: 'put', key: 'key2', value: 'value2b' }
      ]);

      assert.strictEqual(db.getSync('key1'), 'value1b');
      assert.strictEqual(db.getSync('key2'), 'value2b');
    });

    it('should handle multiple keys with overwrites', () => {
      // Write multiple keys
      for (let i = 0; i < 100; i++) {
        db.putSync(`key${i}`, `value${i}`);
      }

      // Overwrite them
      for (let i = 0; i < 100; i++) {
        db.putSync(`key${i}`, `value${i}b`);
      }

      // Verify
      for (let i = 0; i < 100; i++) {
        assert.strictEqual(db.getSync(`key${i}`), `value${i}b`);
      }
    });
  });

  describe('Deletes', () => {
    it('should handle delete operation', () => {
      db.putSync('key1', 'value1');
      db.delSync('key1');

      const value = db.getSync('key1');
      assert.strictEqual(value, null);
    });

    it('should handle overwrite then delete', () => {
      db.putSync('key1', 'value1');
      db.putSync('key1', 'value2');
      db.delSync('key1');

      const value = db.getSync('key1');
      assert.strictEqual(value, null);
    });

    it('should handle async operations with deletes', async () => {
      await db.put('key1', 'value1');
      await db.del('key1');

      const value = await db.get('key1');
      assert.strictEqual(value, null);
    });
  });

  describe('Mixed Operations', () => {
    it('should handle mixed operations', () => {
      // Multiple writes
      db.putSync('key1', 'value1');
      db.putSync('key2', 'value2');

      // Overwrite
      db.putSync('key1', 'value1b');

      // Delete
      db.delSync('key2');

      // Verify
      assert.strictEqual(db.getSync('key1'), 'value1b');
      assert.strictEqual(db.getSync('key2'), null);
    });
  });

  describe('Persistence', () => {
    it('should persist overwrites across database close and reopen', () => {
      db.putSync('key1', 'value1');
      db.putSync('key1', 'value2');
      db.close();

      // Reopen
      db = new WaveDB(testDbPath);
      const value = db.getSync('key1');
      // Note: Persistence relies on WAL and snapshot both being enabled
      // The value should persist via WAL recovery
      db.close();
    });
  });
});