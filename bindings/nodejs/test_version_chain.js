const { WaveDB } = require('./lib/wavedb.js');

// Test: Multiple writes to DIFFERENT keys (no version chains)
console.log('Test: Different keys (no version chains)');
let db1 = new WaveDB('/tmp/test-version-1-' + Date.now());
db1.putSync('key1', 'value1');
db1.putSync('key2', 'value2');  
db1.putSync('key3', 'value3');
console.log('3 different keys written');
db1.close();
console.log('Database 1 closed successfully\n');

// Test: Multiple writes to SAME key (creates version chain)
console.log('Test: Same key overwrite (creates version chain)');
let db2 = new WaveDB('/tmp/test-version-2-' + Date.now());
db2.putSync('key1', 'value1');
console.log('First write complete');
db2.putSync('key1', 'value2');
console.log('Second write (version chain created)');
db2.close();
console.log('Database 2 closed successfully\n');

console.log('All tests passed');
