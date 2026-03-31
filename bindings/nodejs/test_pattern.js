const { WaveDB } = require('./lib/wavedb.js');
const testPath = '/tmp/test-pattern-' + Date.now();
(async () => {
  const db = new WaveDB(testPath);
  
  // Same pattern as persistence test
  // 10 writes
  for (let i = 0; i < 10; i++) {
    await db.put(`key${i}`, `value${i}`);
  }
  
  // 5 overwrites
  for (let i = 0; i < 5; i++) {
    await db.put(`key${i}`, `value${i}b`);
  }
  
  // 5 deletes
  for (let i = 5; i < 10; i++) {
    await db.del(`key${i}`);
  }
  
  console.log('Before close: key2 =', await db.get('key2'));
  db.close();
  
  const db2 = new WaveDB(testPath);
  console.log('After reopen: key2 =', await db2.get('key2'));
  console.log('After reopen: key5 =', await db2.get('key5'));
  db2.close();
})();
