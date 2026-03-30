/**
 * Comprehensive API Test - Verify all documented features work
 */

const assert = require('assert');
const { WaveDB } = require('./lib/wavedb.js');
const fs = require('fs');

function cleanup(path) {
  if (fs.existsSync(path)) {
    fs.rmSync(path, { recursive: true });
  }
}

let passed = 0;
let failed = 0;

async function test(name, fn) {
  try {
    await fn();
    console.log(`✓ ${name}`);
    passed++;
  } catch (err) {
    console.error(`✗ ${name}:`, err.message);
    failed++;
  }
}

(async () => {
  console.log('=== WaveDB Node.js API Comprehensive Test ===\n');

  // Basic Operations
  console.log('--- Basic Operations ---');

  await test('String keys with delimiter', async () => {
    const path = `/tmp/wavedb-test-${Date.now()}`;
    const db = new WaveDB(path);
    await db.put('users/alice/name', 'Alice');
    const value = await db.get('users/alice/name');
    assert.strictEqual(value, 'Alice');
    db.close();
    cleanup(path);
  });

  await test('Array keys', async () => {
    const path = `/tmp/wavedb-test-${Date.now()}`;
    const db = new WaveDB(path);
    await db.put(['users', 'bob', 'name'], 'Bob');
    const value = await db.get(['users', 'bob', 'name']);
    assert.strictEqual(value, 'Bob');
    db.close();
    cleanup(path);
  });

  await test('Custom delimiter', async () => {
    const path = `/tmp/wavedb-test-${Date.now()}`;
    const db = new WaveDB(path, { delimiter: ':' });
    await db.put('users:charlie:name', 'Charlie');
    const value = await db.get('users:charlie:name');
    assert.strictEqual(value, 'Charlie');
    db.close();
    cleanup(path);
  });

  await test('String values', async () => {
    const path = `/tmp/wavedb-test-${Date.now()}`;
    const db = new WaveDB(path);
    await db.put('key', 'test string value');
    const value = await db.get('key');
    assert.strictEqual(value, 'test string value');
    db.close();
    cleanup(path);
  });

  await test('Buffer values', async () => {
    const path = `/tmp/wavedb-test-${Date.now()}`;
    const db = new WaveDB(path);
    const buf = Buffer.from([0x01, 0x02, 0x03, 0x04]);
    await db.put('binary/key', buf);
    const value = await db.get('binary/key');
    assert.deepStrictEqual(value, buf);
    db.close();
    cleanup(path);
  });

  await test('Null for missing keys', async () => {
    const path = `/tmp/wavedb-test-${Date.now()}`;
    const db = new WaveDB(path);
    const value = await db.get('nonexistent');
    assert.strictEqual(value, null);
    db.close();
    cleanup(path);
  });

  // Sync Operations
  console.log('\n--- Sync Operations ---');

  await test('putSync/getSync', async () => {
    const path = `/tmp/wavedb-test-${Date.now()}`;
    const db = new WaveDB(path);
    db.putSync('key', 'value');
    const value = db.getSync('key');
    assert.strictEqual(value, 'value');
    db.close();
    cleanup(path);
  });

  await test('delSync', async () => {
    const path = `/tmp/wavedb-test-${Date.now()}`;
    const db = new WaveDB(path);
    db.putSync('key', 'value');
    db.delSync('key');
    const value = db.getSync('key');
    assert.strictEqual(value, null);
    db.close();
    cleanup(path);
  });

  await test('batchSync', async () => {
    const path = `/tmp/wavedb-test-${Date.now()}`;
    const db = new WaveDB(path);
    db.batchSync([
      { type: 'put', key: 'key1', value: 'value1' },
      { type: 'put', key: 'key2', value: 'value2' }
    ]);
    assert.strictEqual(db.getSync('key1'), 'value1');
    assert.strictEqual(db.getSync('key2'), 'value2');
    db.close();
    cleanup(path);
  });

  // Async Operations with Callbacks
  console.log('\n--- Async Operations with Callbacks ---');

  await test('put with callback', async () => {
    const path = `/tmp/wavedb-test-${Date.now()}`;
    const db = new WaveDB(path);

    // Test callback API
    await new Promise((resolve, reject) => {
      db.put('key', 'value', (err) => {
        if (err) reject(err);
        else resolve();
      });
    });

    const value = await new Promise((resolve, reject) => {
      db.get('key', (err, value) => {
        if (err) reject(err);
        else resolve(value);
      });
    });

    assert.strictEqual(value, 'value');
    db.close();
    cleanup(path);
  });

  // Object Operations
  console.log('\n--- Object Operations ---');

  await test('putObject', async () => {
    const path = `/tmp/wavedb-test-${Date.now()}`;
    const db = new WaveDB(path);
    await db.putObject({
      users: {
        alice: { name: 'Alice', age: '30' }
      }
    }).catch(e => { db.close(); cleanup(path); throw e; });
    assert.strictEqual(await db.get('users/alice/name'), 'Alice');
    assert.strictEqual(await db.get('users/alice/age'), '30');
    db.close();
    cleanup(path);
  });

  await test('getObject', async () => {
    const path = `/tmp/wavedb-test-${Date.now()}`;
    const db = new WaveDB(path);
    await db.put('users/bob/name', 'Bob');
    await db.put('users/bob/age', '25');
    const obj = await db.getObject('users/bob');
    assert.deepStrictEqual(obj, { name: 'Bob', age: '25' });
    db.close();
    cleanup(path);
  });

  await test('Object with arrays', async () => {
    const path = `/tmp/wavedb-test-${Date.now()}`;
    const db = new WaveDB(path);
    await db.putObject({
      users: {
        alice: { roles: ['admin', 'user'] }
      }
    });
    assert.strictEqual(await db.get('users/alice/roles/0'), 'admin');
    assert.strictEqual(await db.get('users/alice/roles/1'), 'user');
    db.close();
    cleanup(path);
  });

  // Batch Operations
  console.log('\n--- Batch Operations ---');

  await test('Large batch', async () => {
    const path = `/tmp/wavedb-test-${Date.now()}`;
    const db = new WaveDB(path);
    const ops = [];
    for (let i = 0; i < 100; i++) {
      ops.push({ type: 'put', key: `key${i}`, value: `value${i}` });
    }
    await db.batch(ops);
    for (let i = 0; i < 100; i++) {
      assert.strictEqual(await db.get(`key${i}`), `value${i}`);
    }
    db.close();
    cleanup(path);
  });

  await test('Batch with mixed operations', async () => {
    const path = `/tmp/wavedb-test-${Date.now()}`;
    const db = new WaveDB(path);
    await db.put('key1', 'value1');
    await db.batch([
      { type: 'put', key: 'key2', value: 'value2' },
      { type: 'del', key: 'key1' }
    ]);
    assert.strictEqual(await db.get('key1'), null);
    assert.strictEqual(await db.get('key2'), 'value2');
    db.close();
    cleanup(path);
  });

  // Streams
  console.log('\n--- Streams ---');

  await test('createReadStream', (done) => {
    const path = `/tmp/wavedb-test-${Date.now()}`;
    const db = new WaveDB(path);
    db.putSync('key1', 'value1');
    db.putSync('key2', 'value2');
    db.putSync('key3', 'value3');

    const results = [];
    db.createReadStream()
      .on('data', ({ key, value }) => {
        results.push({ key, value });
      })
      .on('end', () => {
        try {
          assert.strictEqual(results.length, 3);
          db.close();
          cleanup(path);
          done();
        } catch (e) {
          db.close();
          cleanup(path);
          done(e);
        }
      });
  });

  await test('Stream with start/end range', (done) => {
    const path = `/tmp/wavedb-test-${Date.now()}`;
    const db = new WaveDB(path);
    db.putSync('a', '1');
    db.putSync('b', '2');
    db.putSync('c', '3');
    db.putSync('d', '4');

    const results = [];
    db.createReadStream({ start: 'b', end: 'd' })
      .on('data', ({ key, value }) => {
        results.push({ key, value });
      })
      .on('end', () => {
        try {
          assert.strictEqual(results.length, 2); // 'b' and 'c'
          assert.strictEqual(results[0].key, 'b');
          assert.strictEqual(results[1].key, 'c');
          db.close();
          cleanup(path);
          done();
        } catch (e) {
          db.close();
          cleanup(path);
          done(e);
        }
      });
  });

  await test('Stream with keyAsArray', (done) => {
    const path = `/tmp/wavedb-test-${Date.now()}`;
    const db = new WaveDB(path);
    db.putSync(['users', 'alice', 'name'], 'Alice');

    const results = [];
    db.createReadStream({ keyAsArray: true })
      .on('data', ({ key, value }) => {
        results.push({ key, value });
      })
      .on('end', () => {
        try {
          assert.strictEqual(results.length, 1);
          assert.deepStrictEqual(results[0].key, ['users', 'alice', 'name']);
          assert.strictEqual(results[0].value, 'Alice');
          db.close();
          cleanup(path);
          done();
        } catch (e) {
          db.close();
          cleanup(path);
          done(e);
        }
      });
  });

  // MVCC Operations (overwrites and deletes)
  console.log('\n--- MVCC Operations ---');

  await test('Overwrites', async () => {
    const path = `/tmp/wavedb-test-${Date.now()}`;
    const db = new WaveDB(path);
    await db.put('key', 'value1');
    await db.put('key', 'value2');
    await db.put('key', 'value3');
    const value = await db.get('key');
    assert.strictEqual(value, 'value3');
    db.close();
    cleanup(path);
  });

  await test('Delete after overwrite', async () => {
    const path = `/tmp/wavedb-test-${Date.now()}`;
    const db = new WaveDB(path);
    await db.put('key', 'value1');
    await db.put('key', 'value2');
    await db.del('key');
    const value = await db.get('key');
    assert.strictEqual(value, null);
    db.close();
    cleanup(path);
  });

  await test('Multiple overwrites and deletes', async () => {
    const path = `/tmp/wavedb-test-${Date.now()}`;
    const db = new WaveDB(path);
    for (let i = 0; i < 10; i++) {
      await db.put('key', `value${i}`);
    }
    await db.del('key');
    const value = await db.get('key');
    assert.strictEqual(value, null);
    db.close();
    cleanup(path);
  });

  // Summary
  console.log('\n=== Summary ===');
  console.log(`${passed}/${passed + failed} tests passed`);

  if (failed === 0) {
    console.log('✓ ALL API FEATURES WORKING!');
    process.exit(0);
  } else {
    console.log('✗ Some tests failed');
    process.exit(1);
  }
})();