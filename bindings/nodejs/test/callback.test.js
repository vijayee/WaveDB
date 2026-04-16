const assert = require('assert');
const { WaveDB } = require('../lib/wavedb.js');
const fs = require('fs');
const path = require('path');

describe('WaveDB Callback API', () => {
  let db;
  let testDbPath;

  beforeEach(() => {
    testDbPath = `/tmp/wavedb-cb-test-${Date.now()}-${Math.random().toString(36).substr(2, 9)}`;
    db = new WaveDB(testDbPath);
  });

  afterEach(() => {
    if (db) {
      db.close();
    }
    if (fs.existsSync(testDbPath)) {
      fs.rmSync(testDbPath, { recursive: true });
    }
  });

  describe('getCb', () => {
    it('should get a value via callback', (done) => {
      db.put('cb/key1', 'hello').then(() => {
        db.getCb('cb/key1', (err, value) => {
          assert.ifError(err);
          assert.strictEqual(value, 'hello');
          done();
        });
      });
    });

    it('should return null for missing keys via callback', (done) => {
      db.getCb('cb/missing', (err, value) => {
        assert.ifError(err);
        assert.strictEqual(value, null);
        done();
      });
    });

    it('should accept array keys via callback', (done) => {
      db.put(['cb', 'arr', 'key'], 'arrval').then(() => {
        db.getCb(['cb', 'arr', 'key'], (err, value) => {
          assert.ifError(err);
          assert.strictEqual(value, 'arrval');
          done();
        });
      });
    });

    it('should require a callback argument', () => {
      assert.throws(() => {
        db.getCb('cb/key1');
      }, /callback required/i);
    });
  });

  describe('putCb', () => {
    it('should put a value via callback', (done) => {
      db.putCb('cb/put1', 'world', (err) => {
        assert.ifError(err);
        db.get('cb/put1').then((value) => {
          assert.strictEqual(value, 'world');
          done();
        });
      });
    });

    it('should put with array key via callback', (done) => {
      db.putCb(['cb', 'put2', 'x'], 'y', (err) => {
        assert.ifError(err);
        db.get(['cb', 'put2', 'x']).then((value) => {
          assert.strictEqual(value, 'y');
          done();
        });
      });
    });

    it('should require a callback argument', () => {
      assert.throws(() => {
        db.putCb('cb/key1', 'val');
      }, /callback required/i);
    });
  });

  describe('delCb', () => {
    it('should delete a value via callback', (done) => {
      db.put('cb/del1', 'todelete').then(() => {
        db.delCb('cb/del1', (err) => {
          assert.ifError(err);
          db.get('cb/del1').then((value) => {
            assert.strictEqual(value, null);
            done();
          });
        });
      });
    });

    it('should succeed when deleting a missing key via callback', (done) => {
      db.delCb('cb/nonexistent', (err) => {
        assert.ifError(err);
        done();
      });
    });

    it('should require a callback argument', () => {
      assert.throws(() => {
        db.delCb('cb/key1');
      }, /callback required/i);
    });
  });

  describe('batchCb', () => {
    it('should execute batch operations via callback', (done) => {
      const ops = [
        { type: 'put', key: 'cb/b1', value: 'v1' },
        { type: 'put', key: 'cb/b2', value: 'v2' },
      ];
      db.batchCb(ops, (err) => {
        assert.ifError(err);
        Promise.all([
          db.get('cb/b1'),
          db.get('cb/b2'),
        ]).then(([v1, v2]) => {
          assert.strictEqual(v1, 'v1');
          assert.strictEqual(v2, 'v2');
          done();
        });
      });
    });

    it('should handle mixed put and delete in batch via callback', (done) => {
      db.put('cb/bdel', 'willdelete').then(() => {
        const ops = [
          { type: 'put', key: 'cb/bnew', value: 'newval' },
          { type: 'del', key: 'cb/bdel' },
        ];
        db.batchCb(ops, (err) => {
          assert.ifError(err);
          Promise.all([
            db.get('cb/bnew'),
            db.get('cb/bdel'),
          ]).then(([newval, delval]) => {
            assert.strictEqual(newval, 'newval');
            assert.strictEqual(delval, null);
            done();
          });
        });
      });
    });

    it('should require a callback argument', () => {
      assert.throws(() => {
        db.batchCb([{ type: 'put', key: 'x', value: 'y' }]);
      }, /callback required/i);
    });

    it('should require array of operations', () => {
      assert.throws(() => {
        db.batchCb('not an array', () => {});
      }, /array of operations required/i);
    });
  });

  describe('error handling', () => {
    it('should pass error to callback when db is closed', (done) => {
      db.close();
      db = null;
      // Create a new closed db to test
      const closedPath = `/tmp/wavedb-cb-closed-${Date.now()}`;
      const closedDb = new WaveDB(closedPath);
      closedDb.close();

      closedDb.getCb('any/key', (err, value) => {
        assert(err !== null && err !== undefined);
        fs.rmSync(closedPath, { recursive: true, force: true });
        done();
      });
    });
  });
});