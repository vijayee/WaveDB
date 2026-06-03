const assert = require('assert');
const { WaveDB } = require('../lib/wavedb.js');
const { Subtree } = require('../lib/subtree.js');
const { GraphLayer } = require('../lib/graph.js');
const { GraphQLLayer } = require('../lib/graphql.js');
const fs = require('fs');
const path = require('path');

describe('Subtree', () => {
  let db;
  let testDbPath;

  beforeEach(() => {
    testDbPath = `/tmp/wavedb-subtree-test-${Date.now()}-${Math.random().toString(36).substr(2, 9)}`;
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

  describe('open/close', () => {
    it('should open and close a subtree', () => {
      const st = db.openSubtree('myns');
      assert.strictEqual(st.isClosed, false);
      assert.strictEqual(st.delimiter, '/');
      st.close();
      assert.strictEqual(st.isClosed, true);
    });

    it('should open a subtree with custom delimiter', () => {
      const st = db.openSubtree('myns', ':');
      assert.strictEqual(st.delimiter, ':');
      st.close();
    });

    it('should throw on invalid prefix', () => {
      assert.throws(() => db.openSubtree(''), /Prefix is required/);
      assert.throws(() => db.openSubtree(123), /Prefix is required/);
    });

    it('should throw on invalid delimiter', () => {
      assert.throws(() => db.openSubtree('x', ''), /Delimiter must be a single character/);
      assert.throws(() => db.openSubtree('x', 'ab'), /Delimiter must be a single character/);
    });

    it('should throw when opening subtree on closed db', () => {
      db.close();
      assert.throws(() => db.openSubtree('x'), /Database is closed/);
    });
  });

  describe('sync put/get/del', () => {
    it('should put, get, and delete values in a subtree', () => {
      const st = db.openSubtree('testns');
      st.putSync('key1', 'value1');
      assert.strictEqual(st.getSync('key1'), 'value1');
      st.delSync('key1');
      assert.strictEqual(st.getSync('key1'), null);
      st.close();
    });

    it('should handle array keys', () => {
      const st = db.openSubtree('testns');
      st.putSync(['users', 'alice', 'name'], 'Alice');
      assert.strictEqual(st.getSync(['users', 'alice', 'name']), 'Alice');
      st.close();
    });

    it('should handle binary values', () => {
      const st = db.openSubtree('testns');
      const buf = Buffer.from([0x01, 0x02, 0x03]);
      st.putSync('binkey', buf);
      const result = st.getSync('binkey');
      assert.deepStrictEqual(result, buf);
      st.close();
    });

    it('should return null for missing keys', () => {
      const st = db.openSubtree('testns');
      assert.strictEqual(st.getSync('nonexistent'), null);
      st.close();
    });

    it('should throw when operating on closed subtree', () => {
      const st = db.openSubtree('testns');
      st.close();
      assert.throws(() => st.putSync('k', 'v'), /Subtree is closed/);
      assert.throws(() => st.getSync('k'), /Subtree is closed/);
      assert.throws(() => st.delSync('k'), /Subtree is closed/);
    });
  });

  describe('cross-subtree isolation', () => {
    it('should isolate keys between subtrees with different prefixes', () => {
      const st1 = db.openSubtree('ns1');
      const st2 = db.openSubtree('ns2');

      st1.putSync('samekey', 'from-ns1');
      st2.putSync('samekey', 'from-ns2');

      assert.strictEqual(st1.getSync('samekey'), 'from-ns1');
      assert.strictEqual(st2.getSync('samekey'), 'from-ns2');

      st1.close();
      st2.close();
    });

    it('should not leak subtree keys into the main db namespace', () => {
      const st = db.openSubtree('isolate');
      st.putSync('mykey', 'myvalue');

      // The main db should see the key under the prefixed path
      const mainVal = db.getSync('isolate/mykey');
      assert.strictEqual(mainVal, 'myvalue');

      // But not under the raw key
      assert.strictEqual(db.getSync('mykey'), null);

      st.close();
    });
  });

  describe('batch', () => {
    it('should execute batch operations synchronously', () => {
      const st = db.openSubtree('batchns');
      st.batchSync([
        { type: 'put', key: 'a', value: '1' },
        { type: 'put', key: 'b', value: '2' },
        { type: 'put', key: 'c', value: '3' },
      ]);

      assert.strictEqual(st.getSync('a'), '1');
      assert.strictEqual(st.getSync('b'), '2');
      assert.strictEqual(st.getSync('c'), '3');

      st.batchSync([
        { type: 'del', key: 'b' },
      ]);

      assert.strictEqual(st.getSync('b'), null);
      st.close();
    });
  });

  describe('scan', () => {
    it('should scan keys in the subtree', () => {
      const st = db.openSubtree('scanns');
      st.putSync('users/alice', 'Alice');
      st.putSync('users/bob', 'Bob');
      st.putSync('items/x', 'X');

      const results = st.scanSyncRaw('users');
      assert.ok(results.length >= 2);

      st.close();
    });
  });

  describe('count', () => {
    it('should count entries in the subtree', () => {
      const st = db.openSubtree('countns');
      assert.strictEqual(st.count(), 0);

      st.putSync('key1', 'val1');
      st.putSync('key2', 'val2');
      st.putSync('key3', 'val3');

      assert.strictEqual(st.count(), 3);
      st.close();
    });
  });

  describe('deleteSubtree', () => {
    it('should delete all keys under a prefix', () => {
      const st = db.openSubtree('delns');
      st.putSync('key1', 'val1');
      st.putSync('key2', 'val2');
      st.close();

      db.deleteSubtree('delns');

      // Re-open same prefix, should be empty
      const st2 = db.openSubtree('delns');
      assert.strictEqual(st2.count(), 0);
      st2.close();
    });
  });

  describe('snapshot', () => {
    it('should not throw on snapshot', () => {
      const st = db.openSubtree('snapns');
      st.putSync('key1', 'val1');
      assert.doesNotThrow(() => st.snapshot());
      st.close();
    });
  });
});

describe('Subtree with Graph/GraphQL layers', () => {
  it('should expose _getPtr for cross-addon pointer passing', () => {
    const testPath = `/tmp/wavedb-subtree-ptr-test-${Date.now()}-${Math.random().toString(36).substr(2, 9)}`;
    const db2 = new WaveDB(testPath);
    const st = db2.openSubtree('ptrtest');
    // _getPtr returns a BigInt (the raw C pointer value) for 64-bit safety
    const ptr = st._getPtr();
    assert.strictEqual(typeof ptr, 'bigint');
    assert.ok(ptr > 0n, 'Pointer should be a non-zero BigInt');
    st.close();
    db2.close();
    if (fs.existsSync(testPath)) {
      fs.rmSync(testPath, { recursive: true });
    }
  });
});