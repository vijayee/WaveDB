const assert = require('assert');
const { WaveDB } = require('../lib/wavedb.js');
const fs = require('fs');

async function debug() {
  const testDbPath = `/tmp/wavedb-debug-test-${Date.now()}`;
  console.log('Test path:', testDbPath);

  const db = new WaveDB(testDbPath);

  // Write some data
  await db.put('key0', 'value0');
  await db.put('key0', 'value0b');

  console.log('Before close - checking WAL files:');
  const files = fs.readdirSync(testDbPath);
  console.log('Files in directory:', files);
  const walFiles = files.filter(f => f.endsWith('.wal'));
  console.log('WAL files:', walFiles);
  walFiles.forEach(f => {
    const filePath = `${testDbPath}/${f}`;
    const stats = fs.statSync(filePath);
    console.log(`  ${f}: ${stats.size} bytes`);
  });

  db.close();

  console.log('\nAfter close - checking WAL files:');
  const filesAfter = fs.readdirSync(testDbPath);
  console.log('Files in directory:', filesAfter);
  const walFilesAfter = filesAfter.filter(f => f.endsWith('.wal'));
  console.log('WAL files:', walFilesAfter);
  walFilesAfter.forEach(f => {
    const filePath = `${testDbPath}/${f}`;
    const stats = fs.statSync(filePath);
    console.log(`  ${f}: ${stats.size} bytes`);
  });

  // Reopen
  console.log('\nReopening database...');
  const db2 = new WaveDB(testDbPath);

  console.log('\nAfter reopen - checking WAL files:');
  const filesReopen = fs.readdirSync(testDbPath);
  console.log('Files in directory:', filesReopen);

  const val = await db2.get('key0');
  console.log('Value:', val);
  console.log('Expected: value0b');

  db2.close();

  // Cleanup
  fs.rmSync(testDbPath, { recursive: true });
}

debug().catch(console.error);