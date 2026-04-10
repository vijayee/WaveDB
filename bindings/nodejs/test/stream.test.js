'use strict';

const { WaveDB } = require('../lib/wavedb');
const assert = require('assert');
const fs = require('fs');
const path = require('path');

describe('WaveDB Stream', function() {
  let db;
  let tempDir;

  beforeEach(function() {
    tempDir = fs.mkdtempSync('/tmp/wavedb-stream-test-');
    db = new WaveDB(tempDir);
  });

  afterEach(function() {
    if (db) {
      db.close();
    }
    fs.rmSync(tempDir, { recursive: true, force: true });
  });

  describe('createReadStream', function() {
    it('should return an empty stream for empty database', function(done) {
      const stream = db.createReadStream();
      const entries = [];

      stream.on('data', (entry) => {
        entries.push(entry);
      });

      stream.on('end', () => {
        assert.strictEqual(entries.length, 0);
        done();
      });

      stream.on('error', done);
    });

    it('should iterate over all entries', function(done) {
      // Insert some data
      db.putSync('key1', 'value1');
      db.putSync('key2', 'value2');
      db.putSync('key3', 'value3');

      const stream = db.createReadStream();
      const entries = [];

      stream.on('data', (entry) => {
        entries.push(entry);
      });

      stream.on('end', () => {
        assert.strictEqual(entries.length, 3);

        // Verify entries contain expected data
        const values = entries.map(e => e.value).sort();
        assert.deepStrictEqual(values, ['value1', 'value2', 'value3']);

        done();
      });

      stream.on('error', done);
    });

    it('should support keys-only mode', function(done) {
      db.putSync('key1', 'value1');
      db.putSync('key2', 'value2');

      const stream = db.createReadStream({ values: false });
      const entries = [];

      stream.on('data', (entry) => {
        entries.push(entry);
      });

      stream.on('end', () => {
        assert.strictEqual(entries.length, 2);

        // All values should be null
        entries.forEach(entry => {
          assert.strictEqual(entry.value, null);
          assert.ok(entry.key);
        });

        done();
      });

      stream.on('error', done);
    });

    it('should support values-only mode', function(done) {
      db.putSync('key1', 'value1');
      db.putSync('key2', 'value2');

      const stream = db.createReadStream({ keys: false });
      const entries = [];

      stream.on('data', (entry) => {
        entries.push(entry);
      });

      stream.on('end', () => {
        assert.strictEqual(entries.length, 2);

        // All keys should be null
        entries.forEach(entry => {
          assert.strictEqual(entry.key, null);
          assert.ok(entry.value);
        });

        done();
      });

      stream.on('error', done);
    });

    it('should support custom delimiter', function(done) {
      // Test with a separate database using colon delimiter
      const customDir = fs.mkdtempSync('/tmp/wavedb-stream-custom-');
      const customDb = new WaveDB(customDir, { delimiter: ':' });

      try {
        // Use colon as delimiter
        customDb.putSync('users:alice:name', 'Alice');
        customDb.putSync('users:bob:name', 'Bob');

        // Verify data was stored
        const alice = customDb.getSync('users:alice:name');
        const bob = customDb.getSync('users:bob:name');
        assert.strictEqual(alice, 'Alice', 'Alice should be stored');
        assert.strictEqual(bob, 'Bob', 'Bob should be stored');

        const stream = customDb.createReadStream();
        const entries = [];

        stream.on('data', (entry) => {
          entries.push(entry);
        });

        stream.on('end', () => {
          try {
            assert.strictEqual(entries.length, 2, `Expected 2 entries, got ${entries.length}`);
            customDb.close();
            fs.rmSync(customDir, { recursive: true, force: true });
            done();
          } catch (e) {
            customDb.close();
            fs.rmSync(customDir, { recursive: true, force: true });
            done(e);
          }
        });

        stream.on('error', (err) => {
          customDb.close();
          fs.rmSync(customDir, { recursive: true, force: true });
          done(err);
        });
      } catch (e) {
        customDb.close();
        fs.rmSync(customDir, { recursive: true, force: true });
        done(e);
      }
    });
  });
});