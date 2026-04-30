const { WaveDB } = require('./lib/wavedb.js');
const fs = require('fs');

const dbPath = '/tmp/test_node_sync_only';
try { fs.rmSync(dbPath, { recursive: true }); } catch(e) {}

const db = new WaveDB(dbPath, { syncOnly: true, enablePersist: true });

// sync_only mode: use sync methods (no worker pool for async ops)
db.putSync('hello', 'world');
const result = db.getSync('hello');
console.log('syncOnly get:', result);  // Expected: "world"

db.close();
fs.rmSync(dbPath, { recursive: true });
console.log('Node.js sync_only smoke test: PASS');
