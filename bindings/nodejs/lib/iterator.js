'use strict';

const { Readable } = require('stream');
const { Iterator } = require('../build/Release/wavedb.node');

/**
 * WaveDB Readable Stream Iterator
 *
 * This is a stub implementation. Full functionality requires database_scan API.
 */
class WaveDBIterator extends Readable {
  constructor(db, options = {}) {
    super({ objectMode: true });

    this._db = db;
    this._iterator = new Iterator(options);
    this._start = options.start;
    this._end = options.end;
    this._reverse = options.reverse || false;
    this._keys = options.keys !== false;
    this._values = options.values !== false;
    this._keyAsArray = options.keyAsArray || false;
    this._delimiter = options.delimiter || '/';
    this._reading = false;
  }

  _read(size) {
    if (this._reading) return;
    this._reading = true;

    this._readNext();
  }

  _readNext() {
    // TODO: Implement actual read from native iterator
    // This requires database_scan API

    // For now, push null to end stream
    this.push(null);
    this._reading = false;
  }

  _destroy(err, callback) {
    if (this._iterator) {
      this._iterator.end();
    }
    callback(err);
  }
}

module.exports = { WaveDBIterator };