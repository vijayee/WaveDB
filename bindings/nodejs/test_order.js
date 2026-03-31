const { WaveDB } = require('./lib/wavedb.js');
const testPath = '/tmp/test-order-' + Date.now();
(async () => {
  const db = new WaveDB(testPath);
  
  // Test simple case: write then overwrite same key
  console.log('Writing key0=value0...');
  await db.put('key0', 'value0');
  console.log('Overwriting key0=value0b...');
  await db.put('key0', 'value0b');
  
  // Wait to ensure operations complete
  await new Promise(resolve => setTimeout(resolve, 50));
  
  db.close();
  
  const db2 = new WaveDB(testPath);
  console.log('After reopen: key0 =', await db2.get('key0'));
  db2.close();
})();
