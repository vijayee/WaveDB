const assert = require('assert');
const { WaveDB } = require('./lib/wavedb.js');
const fs = require('fs');

async function test() {
  const testDbPath = `/tmp/wavedb-persist-${Date.now()}`;
  
  try {
    console.log('Test 1: Create database and write data...');
    let db = new WaveDB(testDbPath);
    await db.put('key1', 'value1');
    await db.put('key2', 'value2');
    console.log('Data written');
    
    console.log('Closing database...');
    db.close();
    console.log('Database closed');
    
    console.log('Reopening database...');
    db = new WaveDB(testDbPath);
    const val1 = await db.get('key1');
    const val2 = await db.get('key2');
    
    console.log('Retrieved values:', val1, val2);
    
    assert.strictEqual(val1, 'value1');
    assert.strictEqual(val2, 'value2');
    console.log('Values verified!');
    
    db.close();
    console.log('Test passed!');
  } catch (err) {
    console.error('Test failed:', err);
    process.exit(1);
  } finally {
    if (fs.existsSync(testDbPath)) {
      fs.rmSync(testDbPath, { recursive: true });
    }
  }
}

test();
