const { WaveDB } = require('./lib/wavedb');
const path = require('path');
const fs = require('fs');

// Clean up previous benchmark
const dbPath = '/tmp/wavedb_benchmark';
if (fs.existsSync(dbPath)) {
  fs.rmSync(dbPath, { recursive: true });
}

const db = new WaveDB(dbPath);

async function benchmark() {
  const iterations = 10000;
  const warmup = 100;

  console.log('WaveDB Performance Benchmarks');
  console.log('='.repeat(50));
  console.log(`Node.js ${process.version}`);
  console.log(`Iterations: ${iterations}`);
  console.log('');

  // Warmup
  for (let i = 0; i < warmup; i++) {
    await db.put(`warmup${i}`, `value${i}`);
    await db.get(`warmup${i}`);
  }

  // Async Put
  console.log('Async Operations:');
  console.log('-'.repeat(50));
  let start = process.hrtime.bigint();
  for (let i = 0; i < iterations; i++) {
    await db.put(`key${i}`, `value${i}`);
  }
  let elapsed = Number(process.hrtime.bigint() - start) / 1e9;
  let ops = iterations / elapsed;
  console.log(`  put:       ${ops.toFixed(0).padStart(8)} ops/sec (${(elapsed * 1000).toFixed(2)}ms)`);

  // Async Get
  start = process.hrtime.bigint();
  for (let i = 0; i < iterations; i++) {
    await db.get(`key${i}`);
  }
  elapsed = Number(process.hrtime.bigint() - start) / 1e9;
  ops = iterations / elapsed;
  console.log(`  get:       ${ops.toFixed(0).padStart(8)} ops/sec (${(elapsed * 1000).toFixed(2)}ms)`);

  // Sync Put
  console.log('\nSync Operations:');
  console.log('-'.repeat(50));
  start = process.hrtime.bigint();
  for (let i = 0; i < iterations; i++) {
    db.putSync(`sync${i}`, `value${i}`);
  }
  elapsed = Number(process.hrtime.bigint() - start) / 1e9;
  ops = iterations / elapsed;
  console.log(`  putSync:   ${ops.toFixed(0).padStart(8)} ops/sec (${(elapsed * 1000).toFixed(2)}ms)`);

  // Sync Get
  start = process.hrtime.bigint();
  for (let i = 0; i < iterations; i++) {
    db.getSync(`sync${i}`);
  }
  elapsed = Number(process.hrtime.bigint() - start) / 1e9;
  ops = iterations / elapsed;
  console.log(`  getSync:   ${ops.toFixed(0).padStart(8)} ops/sec (${(elapsed * 1000).toFixed(2)}ms)`);

  // Batch operations
  console.log('\nBatch Operations:');
  console.log('-'.repeat(50));
  const batchSize = 1000;
  const batchOps = [];
  for (let i = 0; i < batchSize; i++) {
    batchOps.push({ type: 'put', key: `batch${i}`, value: `value${i}` });
  }

  start = process.hrtime.bigint();
  for (let i = 0; i < iterations / batchSize; i++) {
    await db.batch(batchOps);
  }
  elapsed = Number(process.hrtime.bigint() - start) / 1e9;
  ops = iterations / elapsed;
  console.log(`  batch:    ${ops.toFixed(0).padStart(8)} ops/sec (${(elapsed * 1000).toFixed(2)}ms, ${batchSize} ops/batch)`);

  // Stream iteration
  console.log('\nStream Operations:');
  console.log('-'.repeat(50));
  start = process.hrtime.bigint();
  let count = 0;
  await new Promise((resolve, reject) => {
    db.createReadStream()
      .on('data', () => { count++; })
      .on('error', reject)
      .on('end', resolve);
  });
  elapsed = Number(process.hrtime.bigint() - start) / 1e9;
  ops = count / elapsed;
  console.log(`  stream:    ${ops.toFixed(0).padStart(8)} entries/sec (${count} entries in ${(elapsed * 1000).toFixed(2)}ms)`);

  console.log('\n' + '='.repeat(50));
  db.close();

  // Cleanup
  if (fs.existsSync(dbPath)) {
    fs.rmSync(dbPath, { recursive: true });
  }
}

benchmark().catch(console.error);