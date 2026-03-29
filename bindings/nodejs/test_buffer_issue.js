const { WaveDB } = require('./lib/wavedb.js');
const fs = require('fs');

async function test() {
  const testDbPath = `/tmp/wavedb-buff-${Date.now()}`;
  
  try {
    console.log('Test: String values...');
    let db = new WaveDB(testDbPath);
    await db.put('key1', 'value1');
    const val1 = await db.get('key1');
    console.log('Before close - type:', typeof val1, 'value:', val1, 'isBuffer:', Buffer.isBuffer(val1));
    
    db.close();
    
    db = new WaveDB(testDbPath);
    const val2 = await db.get('key1');
    console.log('After close - type:', typeof val2, 'value:', val2, 'isBuffer:', Buffer.isBuffer(val2));
    if (Buffer.isBuffer(val2)) {
      console.log('Buffer hex:', val2.toString('hex'));
    }
    
    db.close();
  } catch (err) {
    console.error('Error:', err);
    process.exit(1);
  } finally {
    if (fs.existsSync(testDbPath)) {
      fs.rmSync(testDbPath, { recursive: true });
    }
  }
}

test();
