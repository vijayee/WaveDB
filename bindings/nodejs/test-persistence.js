/**
 * Test persistence across database restarts
 */

const assert = require('assert');
const { WaveDB } = require('./lib/wavedb.js');
const fs = require('fs');

function cleanup(path) {
  if (fs.existsSync(path)) {
    fs.rmSync(path, { recursive: true });
  }
}

async function testPersistence() {
  const path = `/tmp/wavedb-persistence-test-${Date.now()}`;
  console.log('=== Testing Persistence ===\n');

  try {
    // Test 1: Write data, close, reopen, verify
    console.log('Test 1: Write data, close, reopen, verify');
    let db = new WaveDB(path);

    await db.put('key1', 'value1');
    await db.put('key2', 'value2');
    await db.put('key3', 'value3');
    console.log('✓ Wrote 3 key-value pairs');

    db.close();
    console.log('✓ Closed database');

    // Reopen database
    db = new WaveDB(path);
    const val1 = await db.get('key1');
    const val2 = await db.get('key2');
    const val3 = await db.get('key3');

    assert.strictEqual(val1, 'value1');
    assert.strictEqual(val2, 'value2');
    assert.strictEqual(val3, 'value3');
    console.log('✓ All values persisted correctly');

    db.close();
    cleanup(path);
    console.log('✓ Test 1 passed\n');

    // Test 2: Overwrites persist
    console.log('Test 2: Overwrites persist');
    db = new WaveDB(path);

    await db.put('key', 'value1');
    await db.put('key', 'value2');
    await db.put('key', 'value3');
    console.log('✓ Wrote 3 versions to same key');

    db.close();
    console.log('✓ Closed database');

    // Reopen and verify latest value
    db = new WaveDB(path);
    const val = await db.get('key');
    assert.strictEqual(val, 'value3');
    console.log('✓ Latest value persisted:', val);

    db.close();
    cleanup(path);
    console.log('✓ Test 2 passed\n');

    // Test 3: Deletes persist
    console.log('Test 3: Deletes persist');
    db = new WaveDB(path);

    await db.put('key1', 'value1');
    await db.put('key2', 'value2');
    await db.del('key1');
    console.log('✓ Wrote 2 keys, deleted 1');

    db.close();
    console.log('✓ Closed database');

    // Reopen and verify delete
    db = new WaveDB(path);
    const deleted = await db.get('key1');
    const remaining = await db.get('key2');
    assert.strictEqual(deleted, null);
    assert.strictEqual(remaining, 'value2');
    console.log('✓ Deleted key is null:', deleted);
    console.log('✓ Remaining key persisted:', remaining);

    db.close();
    cleanup(path);
    console.log('✓ Test 3 passed\n');

    // Test 4: Batch operations persist
    console.log('Test 4: Batch operations persist');
    db = new WaveDB(path);

    await db.batch([
      { type: 'put', key: 'b1', value: 'v1' },
      { type: 'put', key: 'b2', value: 'v2' },
      { type: 'put', key: 'b3', value: 'v3' }
    ]);
    console.log('✓ Batch wrote 3 keys');

    db.close();
    console.log('✓ Closed database');

    // Reopen and verify batch
    db = new WaveDB(path);
    const b1 = await db.get('b1');
    const b2 = await db.get('b2');
    const b3 = await db.get('b3');
    assert.strictEqual(b1, 'v1');
    assert.strictEqual(b2, 'v2');
    assert.strictEqual(b3, 'v3');
    console.log('✓ All batch values persisted');

    db.close();
    cleanup(path);
    console.log('✓ Test 4 passed\n');

    // Test 5: Sync operations persist
    console.log('Test 5: Sync operations persist');
    db = new WaveDB(path);

    db.putSync('s1', 'sync1');
    db.putSync('s2', 'sync2');
    db.delSync('s1');
    console.log('✓ Sync wrote 2 keys, deleted 1');

    db.close();
    console.log('✓ Closed database');

    // Reopen and verify sync ops
    db = new WaveDB(path);
    const s1 = await db.get('s1');
    const s2 = await db.get('s2');
    assert.strictEqual(s1, null);
    assert.strictEqual(s2, 'sync2');
    console.log('✓ Sync operations persisted correctly');

    db.close();
    cleanup(path);
    console.log('✓ Test 5 passed\n');

    console.log('=== ALL PERSISTENCE TESTS PASSED ===');
    console.log('✓ Data persists across database restarts');
    console.log('✓ Overwrites persist correctly');
    console.log('✓ Deletes persist correctly');
    console.log('✓ Batch operations persist correctly');
    console.log('✓ Sync operations persist correctly');
    process.exit(0);

  } catch (err) {
    console.error('✗ Test failed:', err.message);
    console.error(err.stack);
    process.exit(1);
  }
}

testPersistence();