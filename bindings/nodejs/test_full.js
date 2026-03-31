const { WaveDB } = require('./lib/wavedb.js');
const testPath = '/tmp/test-full-' + Date.now();
(async () => {
  const db = new WaveDB(testPath);
  
  // Multiple writes
  for (let i = 0; i < 10; i++) {
    await db.put(`key${i}`, `value${i}`);
  }
  
  // Overwrites
  for (let i = 0; i < 5; i++) {
    await db.put(`key${i}`, `value${i}b`);
  }
  
  // Deletes
  for (let i = 5; i < 10; i++) {
    await db.del(`key${i}`);
  }
  
  // Check before close
  for (let i = 0; i < 5; i++) {
    const val = await db.get(`key${i}`);
    console.log(`Before close: key${i} = ${val}`);
  }
  
  db.close();
  
  const db2 = new WaveDB(testPath);
  // Check after reopen
  for (let i = 0; i < 5; i++) {
    const val = await db2.get(`key${i}`);
    console.log(`After reopen: key${i} = ${val}`);
  }
  db2.close();
})();
