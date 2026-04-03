'use strict';

const { Readable } = require('stream');

/**
 * WaveDB Readable Stream Iterator
 *
 * Provides a Node.js readable stream interface for iterating over database entries.
 */
class WaveDBIterator extends Readable {
  constructor(nativeIterator, options = {}) {
    super({ objectMode: true });

    // nativeIterator is the native Iterator object created by WaveDB.createReadStream
    this._iterator = nativeIterator;
    this._options = options;
    this._reading = false;
    this._ended = false;
  }

  _read(size) {
    if (this._reading || this._ended) return;
    this._reading = true;

    this._readNext();
  }

  _readNext() {
    if (this._ended) {
      this._reading = false;
      return;
    }

    try {
      const entry = this._iterator.read();

      if (entry === null || entry === undefined) {
        // End of iteration
        this._ended = true;
        this.push(null);
        this._reading = false;
      } else {
        // Push the entry and continue
        this.push(entry);
        this._reading = false;
      }
    } catch (err) {
      this.emit('error', err);
      this.push(null);
      this._reading = false;
    }
  }

  _destroy(err, callback) {
    this._ended = true;

    if (this._iterator) {
      try {
        this._iterator.end();
      } catch (e) {
        // Ignore errors on cleanup
      }
      this._iterator = null;
    }

    callback(err);
  }
}

module.exports = { WaveDBIterator };