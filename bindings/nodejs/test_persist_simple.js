const { WaveDB } = require('./lib/wavedb.js');
const fs = require('fs');
const assert = require('assert');

async function test() {
  const testDbPath = `/tmp/wavedb-test-persist-${Date.now()}`;
  
  try {
    console.log('Creating database...');
    const db = new WaveDB(testDbPath);
    
    console.log('Writing data...');
    await db.put('key1', 'value1');
    await db.put('key2', 'value2');
    
    console.log('Closing database...');
    db.close();
    
    console.log('Reopening database...');
    const db2 = new WaveDB(testDbPath);
    
    console.log('Reading data...');
    const value1 = await db2.get('key1');
    const value2 = await db2.get('key2');
    
    console.log('Value1:', value1);
    console.log('Value2:', value2);
    
    assert.strictEqual(value1, 'value1', 'Value1 should match');
    assert.strictEqual(value2, 'value2', 'Value2 should match');
    
    console.log('Closing second database...');
    db2.close();
    
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
