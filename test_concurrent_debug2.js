const assert = require('assert');
const { WaveDB } = require('./lib/wavedb.js');
const fs = require('fs');

async function test() {
  const testDbPath = `/tmp/wavedb-test-${Date.now()}`;
  
  let db;
  try {
    console.log('Creating database...');
    db = new WaveDB(testDbPath);
    console.log('Database created');
    
    console.log('Test: Two concurrent writes...');
    const writes = [];
    writes.push(db.put('key1', 'value1'));
    writes.push(db.put('key2', 'value2'));
    await Promise.all(writes);
    console.log('✓ Concurrent writes passed');
    
    console.log('Closing database...');
    db.close();
    console.log('Database closed');
    
    console.log('Test completed successfully');
  } catch (err) {
    console.error('Test failed:', err);
    process.exit(1);
  } finally {
    console.log('In finally block');
    if (fs.existsSync(testDbPath)) {
      fs.rmSync(testDbPath, { recursive: true });
    }
  }
}

test().then(() => {
  console.log('Test exited successfully');
}).catch(err => {
  console.error('Unhandled error:', err);
  process.exit(1);
});
