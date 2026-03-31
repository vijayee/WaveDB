const { WaveDB } = require('./lib/wavedb.js');
const fs = require('fs');
const testPath = '/tmp/test-debug-' + Date.now();
(async () => {
  const db = new WaveDB(testPath);
  await db.put('key0', 'value0');
  await db.put('key0', 'value0b');
  console.log('Before close: key0 =', await db.get('key0'));
  
  // List WAL files before close
  const files = fs.readdirSync(testPath);
  console.log('Files before close:', files.filter(f => f.endsWith('.wal')));
  
  db.close();
  
  // List WAL files after close
  const filesAfter = fs.readdirSync(testPath);
  console.log('Files after close:', filesAfter.filter(f => f.endsWith('.wal')));
  
  const db2 = new WaveDB(testPath);
  console.log('After reopen: key0 =', await db2.get('key0'));
  db2.close();
})();
