const { WaveDB } = require('./lib/wavedb.js');
const testPath = '/tmp/test-key2-' + Date.now();
(async () => {
  const db = new WaveDB(testPath);
  
  // Mimic the persistence test for key2 only
  await db.put('key2', 'value2');
  await db.put('key2', 'value2b');
  
  console.log('Before close: key2 =', await db.get('key2'));
  db.close();
  
  const db2 = new WaveDB(testPath);
  console.log('After reopen: key2 =', await db2.get('key2'));
  db2.close();
})();
