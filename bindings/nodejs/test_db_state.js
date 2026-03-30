const { WaveDB } = require('./lib/wavedb.js');
const fs = require('fs');

// Test 1: Open and close without operations
console.log('\n=== Test 1: Open/close without operations ===');
let db1 = new WaveDB('/tmp/test-db-state-1-' + Date.now());
console.log('Created database 1');
console.log('Database 1 pointer:', db1._db);
db1.close();
console.log('Database 1 closed successfully\n');

// Test 2: Open, put, close  
console.log('=== Test 2: Put operation ===');
let db2 = new WaveDB('/tmp/test-db-state-2-' + Date.now());
console.log('Created database 2');
db2.putSync('key1', 'value1');
console.log('Put complete');
console.log('Database 2 pointer:', db2._db);
db2.close();
console.log('Database 2 closed successfully\n');

// Test 3: Open, delete, close
console.log('=== Test 3: Delete operation ===');
let db3 = new WaveDB('/tmp/test-db-state-3-' + Date.now());
console.log('Created database 3');
db3.putSync('key1', 'value1');
console.log('Put complete');
db3.delSync('key1');
console.log('Delete complete');
console.log('Database 3 pointer:', db3._db);

// Try to check database state
try {
  db3.getSync('key1');
  console.log('Get after delete succeeded (returned null or value)');
} catch (e) {
  console.log('Get after delete threw:', e.message);
}

console.log('About to close...');
try {
  db3.close();
  console.log('Database 3 closed successfully');
} catch (e) {
  console.log('Database 3 close threw:', e.message);
}

// Cleanup
for (let path of ['/tmp/test-db-state-1', '/tmp/test-db-state-2', '/tmp/test-db-state-3']) {
  let fullPath = path + '-' + Date.now();
  // Paths already used above, not cleaning up
}
