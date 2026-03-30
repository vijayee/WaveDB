const { WaveDB } = require('./lib/wavedb.js');
const fs = require('fs');

async function test() {
  console.log('Test 1: First database...');
  let db1 = new WaveDB('/tmp/test-db-1');
  await db1.put('key1', 'value1');
  const val1 = await db1.get('key1');
  console.log('Got:', val1);
  db1.close();
  console.log('Database 1 closed');
  
  console.log('\nTest 2: Second database...');
  let db2 = new WaveDB('/tmp/test-db-2');
  await db2.put('key2', 'value2');
  const val2 = await db2.get('key2');
  console.log('Got:', val2);
  db2.close();
  console.log('Database 2 closed');
  
  console.log('\nTest 3: Third database...');
  let db3 = new WaveDB('/tmp/test-db-3');
  await db3.put('key3', 'value3');
  const val3 = await db3.get('key3');
  console.log('Got:', val3);
  db3.close();
  console.log('Database 3 closed');
  
  console.log('\nAll tests passed!');
  
  // Cleanup
  fs.rmSync('/tmp/test-db-1', { recursive: true });
  fs.rmSync('/tmp/test-db-2', { recursive: true });
  fs.rmSync('/tmp/test-db-3', { recursive: true });
}

test().catch(err => {
  console.error('Test failed:', err);
  process.exit(1);
});
