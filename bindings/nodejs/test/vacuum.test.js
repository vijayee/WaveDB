/**
 * Vacuum / compaction binding test
 *
 * Verifies that:
 *   - The `vacuum` config object is parsed by the native binding
 *   - db.vacuum() compacts the page file (file size shrinks)
 *   - Keys remain readable after vacuum
 */

const assert = require('assert');
const { WaveDB } = require('../lib/wavedb.js');
const fs = require('fs');
const path = require('path');
const os = require('os');

describe('Vacuum', function () {
  this.timeout(60000);
  let db;
  let tmpdir;
  let dbDir;
  let pageFilePath;

  beforeEach(() => {
    tmpdir = fs.mkdtempSync(path.join(os.tmpdir(), 'wavedb-nodejs-vacuum-'));
    dbDir = path.join(tmpdir, 'db');
    fs.mkdirSync(dbDir);
    // database_create_with_config treats `location` as a directory and
    // appends `/data.wdbp` for the page file.
    pageFilePath = path.join(dbDir, 'data.wdbp');
  });

  afterEach(() => {
    if (db) {
      try { db.close(); } catch (e) { /* ignore */ }
      db = null;
    }
    if (tmpdir && fs.existsSync(tmpdir)) {
      fs.rmSync(tmpdir, { recursive: true });
    }
  });

  it('should shrink the page file after manual vacuum and keep keys readable', () => {
    // Create DB with custom vacuum config — manual_only so background vacuum
    // does not run automatically, and thresholds are zeroed so the manual
    // call is always eligible. syncOnly avoids MVCC/worker-thread drain
    // interactions, isolating the binding surface under test.
    db = new WaveDB(dbDir, {
      enablePersist: true,
      syncOnly: true,
      vacuum: {
        mode: 'manual_only',
        staleThreshold: 0.30,
        minFileSizeBytes: 0,
        minStaleBytes: 0,
      },
    });

    // Write + overwrite enough to grow the file and create stale regions.
    // Flush after each round so the page file actually holds the CoW'd
    // bnodes — vacuum reclaims stale CoW copies from the page file, not
    // from the in-memory trie (matches the C test pattern). Use enough keys
    // to span multiple 4 KB pages so vacuum has real stale bytes to reclaim.
    const N = 1000;
    for (let i = 0; i < N; i++) {
      db.putSync(`k/${i}`, `v0_${i}`);
    }
    db.flush();
    for (let rep = 1; rep <= 5; rep++) {
      for (let i = 0; i < N; i++) {
        db.putSync(`k/${i}`, `v${rep}_${i}`);
      }
      db.flush();
    }

    const before = fs.statSync(pageFilePath).size;
    assert.ok(before > 4096, `file should have grown past one page: ${before}`);

    // Manual vacuum — must be exposed on the JS wrapper.
    assert.strictEqual(typeof db.vacuum, 'function', 'db.vacuum must be a function');
    db.vacuum();

    const after = fs.statSync(pageFilePath).size;
    assert.ok(after < before, `file should shrink after vacuum: ${before} -> ${after}`);

    // Keys must still be readable after vacuum.
    for (let i = 0; i < N; i++) {
      const v = db.getSync(`k/${i}`);
      assert.strictEqual(v, `v5_${i}`, `key k/${i} should be v5_${i} after vacuum, got ${v}`);
    }
  });

  it('vacuumStatus returns sensible fields', () => {
    db = new WaveDB(dbDir, {
      enablePersist: true,
      syncOnly: true,
      vacuum: {
        mode: 'manual_only',
        staleThreshold: 0.30,
        minFileSizeBytes: 0,
        minStaleBytes: 0,
      },
    });

    // Fresh DB — staleRatio is 0, no vacuum in progress, no cursors.
    const s0 = db.vacuumStatus();
    assert.strictEqual(typeof s0, 'object', 'vacuumStatus must return an object');
    assert.strictEqual(typeof s0.staleRatio, 'number');
    assert.strictEqual(typeof s0.wouldTrigger, 'boolean');
    assert.strictEqual(s0.vacuumInProgress, false);
    assert.strictEqual(s0.openCursorCount, 0);

    // Write + overwrite to generate stale bytes.
    const N = 100;
    for (let i = 0; i < N; i++) db.putSync(`k/${i}`, `v0_${i}`);
    db.flush();
    for (let rep = 1; rep <= 5; rep++) {
      for (let i = 0; i < N; i++) db.putSync(`k/${i}`, `v${rep}_${i}`);
      db.flush();
    }

    const s1 = db.vacuumStatus();
    assert.ok(s1.staleBytes > 0, `staleBytes should be > 0 after overwrites: ${s1.staleBytes}`);
    assert.ok(s1.staleRatio > 0.0, `staleRatio should be > 0: ${s1.staleRatio}`);
    assert.ok(s1.fileSize > 0, `fileSize should be > 0: ${s1.fileSize}`);
    assert.strictEqual(s1.vacuumInProgress, false);
    assert.strictEqual(s1.openCursorCount, 0);
  });
});