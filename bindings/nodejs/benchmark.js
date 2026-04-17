const { WaveDB } = require('./lib/wavedb');
const path = require('path');
const fs = require('fs');
const os = require('os');

const iterations = 10000;
const warmup = 100;

function cleanPath(dbPath) {
  if (fs.existsSync(dbPath)) {
    fs.rmSync(dbPath, { recursive: true });
  }
}

async function warmupDb(db) {
  for (let i = 0; i < warmup; i++) {
    await db.put(`warmup${i}`, `value${i}`);
    await db.get(`warmup${i}`);
  }
}

async function benchSequentialAsync(db, label) {
  console.log(`\nSequential Async (${label}):`);
  console.log('-'.repeat(50));

  let start = process.hrtime.bigint();
  for (let i = 0; i < iterations; i++) {
    await db.put(`key${i}`, `value${i}`);
  }
  let elapsed = Number(process.hrtime.bigint() - start) / 1e9;
  console.log(`  put:       ${(iterations / elapsed).toFixed(0).padStart(8)} ops/sec`);

  start = process.hrtime.bigint();
  for (let i = 0; i < iterations; i++) {
    await db.get(`key${i}`);
  }
  elapsed = Number(process.hrtime.bigint() - start) / 1e9;
  console.log(`  get:       ${(iterations / elapsed).toFixed(0).padStart(8)} ops/sec`);
}

async function benchSync(db, label) {
  console.log(`\nSync Operations (${label}):`);
  console.log('-'.repeat(50));

  let start = process.hrtime.bigint();
  for (let i = 0; i < iterations; i++) {
    db.putSync(`sync${i}`, `value${i}`);
  }
  let elapsed = Number(process.hrtime.bigint() - start) / 1e9;
  console.log(`  putSync:   ${(iterations / elapsed).toFixed(0).padStart(8)} ops/sec`);

  start = process.hrtime.bigint();
  for (let i = 0; i < iterations; i++) {
    db.getSync(`sync${i}`);
  }
  elapsed = Number(process.hrtime.bigint() - start) / 1e9;
  console.log(`  getSync:   ${(iterations / elapsed).toFixed(0).padStart(8)} ops/sec`);
}

async function benchBatch(db) {
  console.log('\nBatch Operations:');
  console.log('-'.repeat(50));
  const batchSize = 1000;
  const batchOps = [];
  for (let i = 0; i < batchSize; i++) {
    batchOps.push({ type: 'put', key: `batch${i}`, value: `value${i}` });
  }

  const start = process.hrtime.bigint();
  for (let i = 0; i < iterations / batchSize; i++) {
    await db.batch(batchOps);
  }
  const elapsed = Number(process.hrtime.bigint() - start) / 1e9;
  console.log(`  batch:    ${(iterations / elapsed).toFixed(0).padStart(8)} ops/sec (${batchSize} ops/batch)`);
}

async function benchConcurrent(db, label) {
  console.log(`\nConcurrent Async (${label}):`);
  console.log('-'.repeat(50));

  const cpuCount = os.cpus().length;
  const threadCounts = [1, 2, 4, 8, 16, 32].filter(t => t <= cpuCount * 2);
  if (!threadCounts.includes(1)) threadCounts.unshift(1);

  for (const c of threadCounts) {
    // Concurrent puts
    const putKeys = [];
    for (let i = 0; i < iterations; i++) {
      putKeys.push(`cp${c}_${i}`);
    }
    let start = process.hrtime.bigint();
    let idx = 0;
    while (idx < iterations) {
      const chunk = putKeys.slice(idx, idx + c).map((k, j) =>
        db.put(k, `v${idx + j}`)
      );
      await Promise.all(chunk);
      idx += c;
    }
    let elapsed = Number(process.hrtime.bigint() - start) / 1e9;
    console.log(`  put (c=${String(c).padStart(2)}):  ${(iterations / elapsed).toFixed(0).padStart(8)} ops/sec`);

    // Concurrent gets
    start = process.hrtime.bigint();
    idx = 0;
    while (idx < iterations) {
      const chunk = putKeys.slice(idx, idx + c).map(k => db.get(k));
      await Promise.all(chunk);
      idx += c;
    }
    elapsed = Number(process.hrtime.bigint() - start) / 1e9;
    console.log(`  get (c=${String(c).padStart(2)}):  ${(iterations / elapsed).toFixed(0).padStart(8)} ops/sec`);
  }
}

async function benchStream(db) {
  console.log('\nStream Operations:');
  console.log('-'.repeat(50));
  const start = process.hrtime.bigint();
  let count = 0;
  await new Promise((resolve, reject) => {
    db.createReadStream()
      .on('data', () => { count++; })
      .on('error', reject)
      .on('end', resolve);
  });
  const elapsed = Number(process.hrtime.bigint() - start) / 1e9;
  console.log(`  stream:    ${(count / elapsed).toFixed(0).padStart(8)} entries/sec (${count} entries)`);
}

async function runBenchmarks(dbPath, opts, label) {
  cleanPath(dbPath);
  const db = new WaveDB(dbPath, opts);
  await warmupDb(db);

  await benchSequentialAsync(db, label);
  await benchSync(db, label);
  await benchBatch(db);
  await benchConcurrent(db, label);
  await benchStream(db);

  db.close();
  cleanPath(dbPath);
}

async function main() {
  console.log('WaveDB Performance Benchmarks');
  console.log('='.repeat(50));
  console.log(`Node.js ${process.version}`);
  console.log(`Iterations: ${iterations}`);
  console.log(`CPUs: ${os.cpus().length}`);

  // ---- In-memory (no persistence) ----
  console.log('\n' + '='.repeat(50));
  console.log('MODE: In-Memory (enablePersist=false)');
  console.log('='.repeat(50));
  await runBenchmarks('/tmp/wavedb_bench_mem', {
    delimiter: '/',
    lruMemoryMb: 50,
    enablePersist: false
  }, 'in-memory');

  // ---- DEBOUNCED WAL ----
  console.log('\n' + '='.repeat(50));
  console.log('MODE: DEBOUNCED WAL (100ms fsync)');
  console.log('='.repeat(50));
  await runBenchmarks('/tmp/wavedb_bench_debounced', {
    delimiter: '/',
    lruMemoryMb: 50,
    wal: { syncMode: 'debounced', debounceMs: 100 }
  }, 'DEBOUNCED');

  // ---- ASYNC WAL ----
  console.log('\n' + '='.repeat(50));
  console.log('MODE: ASYNC WAL (OS cache, no fsync)');
  console.log('='.repeat(50));
  await runBenchmarks('/tmp/wavedb_bench_async', {
    delimiter: '/',
    lruMemoryMb: 50,
    wal: { syncMode: 'async' }
  }, 'ASYNC');

  // ---- IMMEDIATE WAL ----
  console.log('\n' + '='.repeat(50));
  console.log('MODE: IMMEDIATE WAL (fsync per write)');
  console.log('='.repeat(50));
  await runBenchmarks('/tmp/wavedb_bench_immediate', {
    delimiter: '/',
    lruMemoryMb: 50,
    wal: { syncMode: 'immediate' }
  }, 'IMMEDIATE');

  console.log('\n' + '='.repeat(50));
}

main().catch(console.error);