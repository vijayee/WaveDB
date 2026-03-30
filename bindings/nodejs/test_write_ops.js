const { WaveDB } = require('./lib/wavedb.js');

// Test 1: Multiple puts
console.log('Test 1: Multiple puts');
let db1 = new WaveDB('/tmp/test-write-1-' + Date.now());
for (let i = 0; i < 5; i++) {
  db1.putSync('key' + i, 'value' + i);
}
console.log('5 puts complete');
db1.close();
console.log('Database 1 closed successfully\n');

// Test 2: Put then get  
console.log('Test 2: Put then get');
let db2 = new WaveDB('/tmp/test-write-2-' + Date.now());
db2.putSync('key1', 'value1');
db2.getSync('key1');
console.log('Put and get complete');
db2.close();
console.log('Database 2 closed successfully\n');

// Test 3: Put then overwrite
console.log('Test 3: Put then overwrite');
let db3 = new WaveDB('/tmp/test-write-3-' + Date.now());
db3.putSync('key1', 'value1');
db3.putSync('key1', 'value2');
console.log('Overwrite complete');
db3.close();
console.log('Database 3 closed successfully\n');

console.log('All tests passed');
