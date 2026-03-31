const { WaveDB } = require('./lib/wavedb.js');
const testPath = '/tmp/test-version-' + Date.now();
(async () => {
  const db = new WaveDB(testPath);
  
  // Write key0=value0, then key0=value0b (create version chain)
  await db.put('key0', 'value0');
  await db.put('key0', 'value0b');
  
  console.log('Before close: key0 =', await db.get('key0'));
  db.close();
  
  const db2 = new WaveDB(testPath);
  console.log('After reopen: key0 =', await db2.get('key0'));
  db2.close();
})();
