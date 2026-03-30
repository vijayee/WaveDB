const { WaveDB } = require('./lib/wavedb.js');

// Test if we can use WaveDB in a way that never creates version chains
console.log('Test: Use WaveDB without creating version chains');

let db1 = new WaveDB('/tmp/test-legacy-1-' + Date.now());
db1.putSync('key1', 'value1');  // Creates legacy single value
console.log('Put key1=value1 (legacy mode)');
db1.close();
console.log('Database 1 closed successfully\n');

let db2 = new WaveDB('/tmp/test-legacy-2-' + Date.now());
db2.putSync('key1', 'value1');  
console.log('Put key1=value1');
db2.putSync('key2', 'value2');  // Different key, no version chain
console.log('Put key2=value2 (no version chain)');
db2.close();
console.log('Database 2 closed successfully\n');

let db3 = new WaveDB('/tmp/test-legacy-3-' + Date.now());
db3.putSync('key1', 'value1');
console.log('Put key1=value1');
db3.delSync('key1');  // Delete creates version chain with tombstone
console.log('Delete key1 (creates version chain)');
db3.close();
console.log('Database 3 closed successfully\n');

console.log('All tests passed!');
