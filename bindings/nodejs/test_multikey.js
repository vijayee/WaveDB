const { WaveDB } = require('./lib/wavedb.js');
const testPath = '/tmp/test-multikey-' + Date.now();
(async () => {
  const db = new WaveDB(testPath);
  
  // Mimic the persistence test exactly
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
  
  console.log('Before close:');
  for (let i = 0; i < 10; i++) {
    console.log(`  key${i} = ${await db.get(`key${i}`)}`);
  }
  
  db.close();
  
  const db2 = new WaveDB(testPath);
  console.log('After reopen:');
  for (let i = 0; i < 10; i++) {
    console.log(`  key${i} = ${await db2.get(`key${i}`)}`);
  }
  db2.close();
})();
