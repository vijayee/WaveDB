const { WaveDB } = require('./lib/wavedb.js');
const fs = require('fs');

let testDbPath = '/tmp/wavedb-test-crash-' + Date.now();
let db = new WaveDB(testDbPath);

db.putSync('key1', 'value1');
db.delSync('key1');
db.close();

if (fs.existsSync(testDbPath)) {
  fs.rmSync(testDbPath, { recursive: true });
}
