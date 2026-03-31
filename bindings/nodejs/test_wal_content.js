const { WaveDB } = require('./lib/wavedb.js');
const fs = require('fs');
const path = require('path');
const testPath = '/tmp/test-wal-content-' + Date.now();
(async () => {
  const db = new WaveDB(testPath);
  
  // Write operations
  console.log('Writing key0=value0...');
  await db.put('key0', 'value0');
  console.log('Writing key0=value0b...');
  await db.put('key0', 'value0b');
  
  console.log('Before close: key0 =', await db.get('key0'));
  
  // List WAL files before close
  const files = fs.readdirSync(testPath);
  const walFiles = files.filter(f => f.endsWith('.wal'));
  console.log('WAL files:', walFiles);
  
  db.close();
  
  // Check WAL files after close
  const filesAfter = fs.readdirSync(testPath);
  const walFilesAfter = filesAfter.filter(f => f.endsWith('.wal'));
  console.log('WAL files after close:', walFilesAfter);
  
  const db2 = new WaveDB(testPath);
  console.log('After reopen: key0 =', await db2.get('key0'));
  db2.close();
})();
