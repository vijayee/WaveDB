const assert = require('assert');
const { WaveDB } = require('../lib/wavedb.js');
const fs = require('fs');
const path = require('path');

describe('WaveDB', () => {
  let db;
  let testDbPath;

  beforeEach(() => {
    testDbPath = `/tmp/wavedb-test-${Date.now()}-${Math.random().toString(36).substr(2, 9)}`;
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

  describe('put/get (async)', () => {
    it('should put and get a value', async () => {
      await db.put('users/alice/name', 'Alice');
      const value = await db.get('users/alice/name');
      assert.strictEqual(value, 'Alice');
    });

    it('should handle string keys with delimiter', async () => {
      await db.put('users/bob/age', '30');
      const value = await db.get('users/bob/age');
      assert.strictEqual(value, '30');
    });

    it('should handle array keys', async () => {
      await db.put(['users', 'charlie', 'name'], 'Charlie');
      const value = await db.get(['users', 'charlie', 'name']);
      assert.strictEqual(value, 'Charlie');
    });

    it('should handle binary values', async () => {
      const buf = Buffer.from([0x01, 0x02, 0x03, 0x04]);
      await db.put('binary/key', buf);
      const result = await db.get('binary/key');
      assert.deepStrictEqual(result, buf);
    });

    it('should return null for missing keys', async () => {
      const value = await db.get('missing/key');
      assert.strictEqual(value, null);
    });

    it('should support callback pattern', (done) => {
      db.put('test/key', 'value', (err) => {
        assert.ifError(err);
        db.get('test/key', (err, value) => {
          assert.ifError(err);
          assert.strictEqual(value, 'value');
          done();
        });
      });
    });
  });

  describe('put/get (sync)', () => {
    it('should put and get a value synchronously', () => {
      db.putSync('users/alice/name', 'Alice');
      const value = db.getSync('users/alice/name');
      assert.strictEqual(value, 'Alice');
    });

    it('should handle array keys synchronously', () => {
      db.putSync(['users', 'bob', 'age'], '30');
      const value = db.getSync(['users', 'bob', 'age']);
      assert.strictEqual(value, '30');
    });
  });

  describe('del (async)', () => {
    it('should delete a value', async () => {
      await db.put('test/key', 'value');
      await db.del('test/key');
      const value = await db.get('test/key');
      assert.strictEqual(value, null);
    });
  });

  describe('del (sync)', () => {
    it('should delete a value synchronously', () => {
      db.putSync('test/key', 'value');
      db.delSync('test/key');
      const value = db.getSync('test/key');
      assert.strictEqual(value, null);
    });
  });

  describe('batch (async)', () => {
    it('should execute batch operations', async () => {
      await db.batch([
        { type: 'put', key: 'users/alice/name', value: 'Alice' },
        { type: 'put', key: 'users/bob/name', value: 'Bob' },
        { type: 'del', key: 'users/charlie/name' }
      ]);

      assert.strictEqual(await db.get('users/alice/name'), 'Alice');
      assert.strictEqual(await db.get('users/bob/name'), 'Bob');
      assert.strictEqual(await db.get('users/charlie/name'), null);
    });

    it('should handle mixed operations in batch', async () => {
      await db.put('key1', 'value1');
      await db.put('key2', 'value2');

      await db.batch([
        { type: 'put', key: 'key3', value: 'value3' },
        { type: 'del', key: 'key1' }
      ]);

      assert.strictEqual(await db.get('key1'), null);
      assert.strictEqual(await db.get('key2'), 'value2');
      assert.strictEqual(await db.get('key3'), 'value3');
    });
  });

  describe('batch (sync)', () => {
    it('should execute batch operations synchronously', () => {
      db.batchSync([
        { type: 'put', key: 'key1', value: 'value1' },
        { type: 'put', key: 'key2', value: 'value2' }
      ]);

      assert.strictEqual(db.getSync('key1'), 'value1');
      assert.strictEqual(db.getSync('key2'), 'value2');
    });
  });

  describe('delimiter option', () => {
    it('should support custom delimiter', () => {
      const dbCustom = new WaveDB('/tmp/test-custom-delim', { delimiter: ':' });
      dbCustom.putSync('users:alice:name', 'Alice');
      const value = dbCustom.getSync('users:alice:name');
      assert.strictEqual(value, 'Alice');
      dbCustom.close();
      fs.rmSync('/tmp/test-custom-delim', { recursive: true });
    });
  });

  describe('error handling', () => {
    it('should throw TypeError for missing key', async () => {
      try {
        await db.put();
        assert.fail('Should have thrown');
      } catch (err) {
        assert(err instanceof TypeError);
      }
    });

    it('should throw TypeError for missing value in put', async () => {
      try {
        await db.put('key');
        assert.fail('Should have thrown');
      } catch (err) {
        assert(err instanceof TypeError);
      }
    });

    it('should throw TypeError for invalid batch operations', async () => {
      try {
        await db.batch([{ type: 'invalid' }]);
        assert.fail('Should have thrown');
      } catch (err) {
        assert(err instanceof TypeError);
      }
    });
  });
});