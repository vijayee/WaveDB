const { WaveDB } = require('./lib/wavedb.js');
const testPath = '/tmp/test-await-' + Date.now();
(async () => {
  const db = new WaveDB(testPath);
  
  // Write operations
  for (let i = 0; i < 10; i++) {
    await db.put(`key${i}`, `value${i}`);
    console.log(`Wrote key${i}=value${i}`);
  }
  
  // Overwrites
  for (let i = 0; i < 5; i++) {
    await db.put(`key${i}`, `value${i}b`);
    console.log(`Overwrote key${i}=value${i}b`);
  }
  
  // Deletes
  for (let i = 5; i < 10; i++) {
    await db.del(`key${i}`);
    console.log(`Deleted key${i}`);
  }
  
  console.log('Before close: waiting 100ms for operations to complete...');
  await new Promise(resolve => setTimeout(resolve, 100));
  
  db.close();
  
  const db2 = new WaveDB(testPath);
  console.log('After reopen:');
  for (let i = 0; i < 10; i++) {
    const val = await db2.get(`key${i}`);
    console.log(`  key${i} = ${val}`);
  }
  db2.close();
})();
