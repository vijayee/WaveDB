const { WaveDB } = require('./lib/wavedb.js');
const fs = require('fs');

const path = `/tmp/test-simple-${Date.now()}`;
let db = new WaveDB(path);

console.log('Writing single key...');
db.putSync('key', 'value');

console.log('Closing database...');
db.close();

console.log('Reopening database...');
db = new WaveDB(path);

const val = db.getSync('key');
console.log('Value:', val);

db.close();
fs.rmSync(path, { recursive: true });

console.log('✓ Simple persistence works');
