const { WaveDB } = require('./lib/wavedb.js');
const testPath = '/tmp/test-simple-' + Date.now();
(async () => {
  const db = new WaveDB(testPath);
  await db.put('key1', 'value1');
  await db.put('key1', 'value2');
  console.log('Before close: key1 =', await db.get('key1'));
  db.close();
  
  const db2 = new WaveDB(testPath);
  console.log('After reopen: key1 =', await db2.get('key1'));
  db2.close();
})();
