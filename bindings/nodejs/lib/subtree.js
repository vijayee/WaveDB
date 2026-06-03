'use strict';

const { WaveDBError, NotFoundError, IOError, convertError } = require('./wavedb.js');

class Subtree {
  constructor(nativeSubtree, delimiter = '/') {
    this._st = nativeSubtree;
    this._delimiter = delimiter;
    this._closed = false;
  }

  get delimiter() { return this._delimiter; }
  get isClosed() { return this._closed; }

  /**
   * Store a key-value pair synchronously
   *
   * @param {string|Array} key - Key path
   * @param {string|Buffer} value - Value to store
   * @throws {WaveDBError} If operation fails
   */
  putSync(key, value) {
    if (this._closed) throw new IOError('Subtree is closed');
    try {
      this._st.putSync(key, value);
    } catch (err) {
      throw convertError(err);
    }
  }

  /**
   * Retrieve a value by key synchronously
   *
   * @param {string|Array} key - Key path
   * @returns {string|Buffer|null} The value, or null if not found
   * @throws {WaveDBError} If operation fails
   */
  getSync(key) {
    if (this._closed) throw new IOError('Subtree is closed');
    try {
      return this._st.getSync(key);
    } catch (err) {
      throw convertError(err);
    }
  }

  /**
   * Delete a key-value pair synchronously
   *
   * @param {string|Array} key - Key path
   * @throws {WaveDBError} If operation fails
   */
  delSync(key) {
    if (this._closed) throw new IOError('Subtree is closed');
    try {
      this._st.delSync(key);
    } catch (err) {
      throw convertError(err);
    }
  }

  /**
   * Execute multiple operations atomically synchronously
   *
   * @param {Array<Object>} ops - Array of operations {type: 'put'|'del', key: ..., value: ...}
   * @throws {WaveDBError} If operation fails
   */
  batchSync(ops) {
    if (this._closed) throw new IOError('Subtree is closed');
    try {
      this._st.batchSync(ops);
    } catch (err) {
      throw convertError(err);
    }
  }

  /**
   * Scan the subtree for keys matching a prefix synchronously
   *
   * @param {string|Array} [prefix] - Key prefix to match
   * @returns {Array<{key: string|Buffer, value: string|Buffer}>} Array of results
   * @throws {WaveDBError} If operation fails
   */
  scanSyncRaw(prefix) {
    if (this._closed) throw new IOError('Subtree is closed');
    try {
      if (prefix !== undefined) {
        return this._st.scanSyncRaw(prefix);
      }
      return this._st.scanSyncRaw();
    } catch (err) {
      throw convertError(err);
    }
  }

  /**
   * Count the number of entries in the subtree
   *
   * @returns {number} Number of entries
   */
  count() {
    if (this._closed) throw new IOError('Subtree is closed');
    return this._st.count();
  }

  /**
   * Snapshot the subtree's underlying database
   *
   * @throws {WaveDBError} If operation fails
   */
  snapshot() {
    if (this._closed) throw new IOError('Subtree is closed');
    try {
      this._st.snapshot();
    } catch (err) {
      throw convertError(err);
    }
  }

  /**
   * Store a key-value pair asynchronously
   *
   * @param {string|Array} key - Key path
   * @param {string|Buffer} value - Value to store
   * @returns {Promise<void>}
   */
  async put(key, value) {
    if (this._closed) throw new IOError('Subtree is closed');
    return this._st.put(key, value);
  }

  /**
   * Retrieve a value by key asynchronously
   *
   * @param {string|Array} key - Key path
   * @returns {Promise<string|Buffer|null>}
   */
  async get(key) {
    if (this._closed) throw new IOError('Subtree is closed');
    return this._st.get(key);
  }

  /**
   * Delete a key-value pair asynchronously
   *
   * @param {string|Array} key - Key path
   * @returns {Promise<void>}
   */
  async del(key) {
    if (this._closed) throw new IOError('Subtree is closed');
    return this._st.del(key);
  }

  /**
   * Close the subtree and release resources.
   * Does NOT close or destroy the underlying database.
   */
  close() {
    if (!this._closed) {
      this._st.close();
      this._closed = true;
    }
  }
}

module.exports = { Subtree };