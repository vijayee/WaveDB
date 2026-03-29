const { WaveDB } = require('./lib/wavedb.js');
const fs = require('fs');

async function test() {
  const testDbPath = `/tmp/wavedb-test-close-${Date.now()}`;
  
  try {
    const db = new WaveDB(testDbPath);
    
    console.log('Starting concurrent operations...');
    const ops = [];
    for (let i = 0; i < 100; i++) {
      ops.push(db.put(`key${i}`, `value${i}`));
    }
    
    console.log('Waiting for operations to complete...');
    await Promise.all(ops);
    console.log('All operations completed');
    
    console.log('Closing database...');
    db.close();
    console.log('Database closed');
    
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
