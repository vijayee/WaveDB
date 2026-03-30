const { WaveDB } = require('./lib/wavedb.js');

// Test: Create multiple databases to see if the issue is with a specific database
console.log('Creating databases without operations');
for (let i = 0; i < 10; i++) {
  let db = new WaveDB('/tmp/test-multi-db-' + i + '-' + Date.now());
  db.close();
}
console.log('10 databases created and closed successfully\n');

// Test: Create databases with puts
console.log('Creating databases with puts');
for (let i = 0; i < 5; i++) {
  let db = new WaveDB('/tmp/test-put-db-' + i + '-' + Date.now());
  db.putSync('key1', 'value1');
  db.close();
}
console.log('5 databases with puts closed successfully\n');

// Test: Create databases with overwrites (version chains)
console.log('Creating databases with overwrites');
for (let i = 0; i < 3; i++) {
  console.log('Creating database', i);
  let db = new WaveDB('/tmp/test-overwrite-db-' + i + '-' + Date.now());
  db.putSync('key1', 'value1');
  console.log('First write done');
  db.putSync('key1', 'value2');
  console.log('Second write done');
  db.close();
  console.log('Database', i, 'closed');
}
console.log('All tests passed');
