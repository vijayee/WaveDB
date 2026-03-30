const { WaveDB } = require('./lib/wavedb.js');

// Test: Check if thread-local WAL state is the issue
console.log('Test 1: No thread-local WAL (open/close only)');
let db1 = new WaveDB('/tmp/test-tl-1-' + Date.now());
db1.close();
console.log('Test 1 passed\n');

// Test: Put creates thread-local WAL
console.log('Test 2: Put operation (creates thread-local WAL)');
let db2 = new WaveDB('/tmp/test-tl-2-' + Date.now());
db2.putSync('key1', 'value1');
console.log('Put complete, thread-local WAL created');
db2.close();
console.log('Test 2 passed\n');

// Test: Overwrite creates version chain
console.log('Test 3: Overwrite (creates version chain + thread-local WAL)');
let db3 = new WaveDB('/tmp/test-tl-3-' + Date.now());
db3.putSync('key1', 'value1');
db3.putSync('key1', 'value2');
console.log('Overwrite complete');
db3.close();
console.log('Test 3 passed\n');

console.log('All tests passed!');
