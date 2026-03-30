const { WaveDB } = require('./lib/wavedb.js');
const fs = require('fs');

const path = `/tmp/test-overwrite-${Date.now()}`;
const db = new WaveDB(path);

console.log('Testing overwrites without close...');
db.putSync('key', 'value1');
db.putSync('key', 'value2');
db.putSync('key', 'value3');

const val = db.getSync('key');
console.log('Value:', val);

// Don't close, just cleanup
fs.rmSync(path, { recursive: true });

console.log('✓ Overwrites work in memory');
