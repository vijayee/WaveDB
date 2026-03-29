const { WaveDB } = require('./lib/wavedb.js');
const fs = require('fs');

async function test() {
  const testDbPath = `/tmp/wavedb-test-simple-${Date.now()}`;
  
  try {
    const db = new WaveDB(testDbPath);
    
    console.log('Test: concurrent reads after writes...');
    for (let i = 0; i < 10; i++) {
      await db.put(`key${i}`, `value${i}`);
    }
    
    const reads = [];
    for (let i = 0; i < 10; i++) {
      reads.push(db.get(`key${i}`));
    }
    
    const values = await Promise.all(reads);
    console.log('All reads completed');
    
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
