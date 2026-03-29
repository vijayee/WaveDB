const assert = require('assert');
const { WaveDB } = require('./lib/wavedb.js');
const fs = require('fs');

let db;
let testDbPath;

async function runTests() {
  console.log('Test 1: concurrent writes');
  testDbPath = `/tmp/wavedb-test-${Date.now()}`;
  db = new WaveDB(testDbPath);
  
  const writes = [];
  for (let i = 0; i < 100; i++) {
    writes.push(db.put(`key${i}`, `value${i}`));
  }
  await Promise.all(writes);
  console.log('✓ concurrent writes passed');
  
  console.log('Test 2: concurrent reads');
  const reads = [];
  for (let i = 0; i < 50; i++) {
    reads.push(db.get(`key${i}`));
  }
  await Promise.all(reads);
  console.log('✓ concurrent reads passed');
  
  console.log('Test 3: mixed concurrent operations');
  const operations = [];
  for (let i = 0; i < 30; i++) {
    operations.push(db.put(`key${i}`, `value${i}`));
  }
  for (let i = 0; i < 20; i++) {
    operations.push(db.get(`key${i}`));
  }
  for (let i = 30; i < 40; i++) {
    operations.push(db.put(`delkey${i}`, `value${i}`));
    operations.push(db.del(`delkey${i}`));
  }
  await Promise.all(operations);
  console.log('✓ mixed concurrent operations passed');
  
  console.log('Closing database...');
  db.close();
  console.log('✓ Database closed');
  
  if (fs.existsSync(testDbPath)) {
    fs.rmSync(testDbPath, { recursive: true });
  }
  
  console.log('All tests passed!');
}

runTests().catch(err => {
  console.error('Test failed:', err);
  process.exit(1);
});
