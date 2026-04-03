'use strict';

const { WaveDB: WaveDBNative } = require('../build/Release/wavedb.node');
const { WaveDBIterator } = require('./iterator.js');

/**
 * Base error class for all WaveDB errors
 */
class WaveDBError extends Error {
  constructor(message) {
    super(message);
    this.name = 'WaveDBError';
  }
}

/**
 * Error thrown when a key is not found
 */
class NotFoundError extends WaveDBError {
  constructor(message = 'Key not found') {
    super(message);
    this.name = 'NotFoundError';
  }
}

/**
 * Error thrown when a path is invalid
 */
class InvalidPathError extends WaveDBError {
  constructor(message = 'Invalid path') {
    super(message);
    this.name = 'InvalidPathError';
  }
}

/**
 * Error thrown when an I/O operation fails
 */
class IOError extends WaveDBError {
  constructor(message = 'I/O error') {
    super(message);
    this.name = 'IOError';
  }
}

/**
 * Convert native error to custom error type
 */
function convertError(err) {
  if (!err) return null;

  // Preserve TypeError from native code
  if (err instanceof TypeError) {
    return err;
  }

  const message = err.message || String(err);

  if (message.includes('NOT_FOUND') || message.includes('Key not found')) {
    return new NotFoundError(message);
  }
  if (message.includes('INVALID_PATH') || message.includes('Invalid path')) {
    return new InvalidPathError(message);
  }
  if (message.includes('IO_ERROR') || message.includes('I/O error') || message.includes('DATABASE_CLOSED')) {
    return new IOError(message);
  }

  return new WaveDBError(message);
}

/**
 * WaveDB database wrapper class
 *
 * Provides both async (Promise/callback) and sync methods for database operations.
 */
class WaveDB {
  constructor(path, options = {}) {
    if (typeof path !== 'string' || !path) {
      throw new InvalidPathError('Database path is required');
    }

    this._delimiter = options.delimiter || '/';
    this._path = path;
    this._db = new WaveDBNative(path, { delimiter: this._delimiter });
    this._closed = false;
  }

  /**
   * Get the database path
   * @returns {string} Database path
   */
  get path() {
    return this._path;
  }

  /**
   * Get the delimiter used for path parsing
   * @returns {string} Delimiter character
   */
  get delimiter() {
    return this._delimiter;
  }

  /**
   * Check if the database is closed
   * @returns {boolean} True if closed
   */
  get isClosed() {
    return this._closed;
  }

  /**
   * Store a key-value pair asynchronously
   *
   * @param {string|Array} key - Key path (string with delimiter or array of identifiers)
   * @param {string|Buffer} value - Value to store
   * @param {Function} [callback] - Optional callback(err)
   * @returns {Promise<void>}
   */
  put(key, value, callback) {
    // Validate required parameters
    if (value === undefined || value === null) {
      const error = new TypeError('Value is required for put operation');
      if (typeof callback === 'function') {
        callback(error);
        return Promise.reject(error);
      }
      return Promise.reject(error);
    }

    const promise = new Promise((resolve, reject) => {
      try {
        this._db.put(key, value, (err) => {
          if (err) {
            reject(convertError(err));
          } else {
            resolve();
          }
        });
      } catch (err) {
        reject(convertError(err));
      }
    });

    if (typeof callback === 'function') {
      promise.then(
        () => callback(null),
        (err) => callback(err)
      );
    }

    return promise;
  }

  /**
   * Retrieve a value by key asynchronously
   *
   * @param {string|Array} key - Key path (string with delimiter or array of identifiers)
   * @param {Function} [callback] - Optional callback(err, value)
   * @returns {Promise<string|Buffer|null>}
   */
  get(key, callback) {
    const promise = new Promise((resolve, reject) => {
      try {
        this._db.get(key, (err, value) => {
          if (err) {
            reject(convertError(err));
          } else {
            resolve(value);
          }
        });
      } catch (err) {
        reject(convertError(err));
      }
    });

    if (typeof callback === 'function') {
      promise.then(
        (value) => callback(null, value),
        (err) => callback(err, null)
      );
    }

    return promise;
  }

  /**
   * Delete a key-value pair asynchronously
   *
   * @param {string|Array} key - Key path (string with delimiter or array of identifiers)
   * @param {Function} [callback] - Optional callback(err)
   * @returns {Promise<void>}
   */
  del(key, callback) {
    const promise = new Promise((resolve, reject) => {
      try {
        this._db.del(key, (err) => {
          if (err) {
            reject(convertError(err));
          } else {
            resolve();
          }
        });
      } catch (err) {
        reject(convertError(err));
      }
    });

    if (typeof callback === 'function') {
      promise.then(
        () => callback(null),
        (err) => callback(err)
      );
    }

    return promise;
  }

  /**
   * Execute multiple operations atomically asynchronously
   *
   * @param {Array<Object>} ops - Array of operations {type: 'put'|'del', key: ..., value: ...}
   * @param {Function} [callback] - Optional callback(err)
   * @returns {Promise<void>}
   */
  batch(ops, callback) {
    const promise = new Promise((resolve, reject) => {
      try {
        this._db.batch(ops, (err) => {
          if (err) {
            reject(convertError(err));
          } else {
            resolve();
          }
        });
      } catch (err) {
        reject(convertError(err));
      }
    });

    if (typeof callback === 'function') {
      promise.then(
        () => callback(null),
        (err) => callback(err)
      );
    }

    return promise;
  }

  /**
   * Store a key-value pair synchronously
   *
   * @param {string|Array} key - Key path (string with delimiter or array of identifiers)
   * @param {string|Buffer} value - Value to store
   * @throws {WaveDBError} If operation fails
   */
  putSync(key, value) {
    try {
      this._db.putSync(key, value);
    } catch (err) {
      throw convertError(err);
    }
  }

  /**
   * Retrieve a value by key synchronously
   *
   * @param {string|Array} key - Key path (string with delimiter or array of identifiers)
   * @returns {string|Buffer|null} The value, or null if not found
   * @throws {WaveDBError} If operation fails
   */
  getSync(key) {
    try {
      return this._db.getSync(key);
    } catch (err) {
      throw convertError(err);
    }
  }

  /**
   * Delete a key-value pair synchronously
   *
   * @param {string|Array} key - Key path (string with delimiter or array of identifiers)
   * @throws {WaveDBError} If operation fails
   */
  delSync(key) {
    try {
      this._db.delSync(key);
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
    try {
      this._db.batchSync(ops);
    } catch (err) {
      throw convertError(err);
    }
  }

  /**
   * Store a JSON object asynchronously
   *
   * @param {string|Array} key - Key path
   * @param {Object} obj - Object to store
   * @param {Function} [callback] - Optional callback(err)
   * @returns {Promise<void>}
   */
  putObject(key, obj, callback) {
    const promise = new Promise((resolve, reject) => {
      try {
        this._db.putObject(key, obj, (err) => {
          if (err) {
            reject(convertError(err));
          } else {
            resolve();
          }
        });
      } catch (err) {
        reject(convertError(err));
      }
    });

    if (typeof callback === 'function') {
      promise.then(
        () => callback(null),
        (err) => callback(err)
      );
    }

    return promise;
  }

  /**
   * Retrieve a JSON object asynchronously
   *
   * @param {string|Array} key - Key path
   * @param {Function} [callback] - Optional callback(err, obj)
   * @returns {Promise<Object|null>}
   */
  getObject(key, callback) {
    const promise = new Promise((resolve, reject) => {
      try {
        this._db.getObject(key, (err, obj) => {
          if (err) {
            reject(convertError(err));
          } else {
            resolve(obj);
          }
        });
      } catch (err) {
        reject(convertError(err));
      }
    });

    if (typeof callback === 'function') {
      promise.then(
        (obj) => callback(null, obj),
        (err) => callback(err, null)
      );
    }

    return promise;
  }

  /**
   * Create a read stream
   *
   * @param {Object} [options] - Stream options
   *   @param {string|Array} [options.start] - Start path (inclusive)
   *   @param {string|Array} [options.end] - End path (exclusive)
   *   @param {boolean} [options.reverse=false] - Scan in reverse order
   *   @param {boolean} [options.keys=true] - Include keys in results
   *   @param {boolean} [options.values=true] - Include values in results
   *   @param {boolean} [options.keyAsArray=false] - Return keys as arrays
   *   @param {string} [options.delimiter='/'] - Path delimiter
   * @returns {WaveDBIterator} Readable stream
   */
  createReadStream(options = {}) {
    if (this._closed) {
      throw new IOError('Database is closed');
    }
    // Merge default delimiter with options
    const iterOptions = {
      ...options,
      delimiter: options.delimiter || this._delimiter
    };
    // Create native iterator through the native createReadStream
    const nativeIterator = this._db.createReadStream(iterOptions);
    // Wrap in JavaScript stream
    return new WaveDBIterator(nativeIterator, iterOptions);
  }

  /**
   * Close the database
   */
  close() {
    if (this._db && !this._closed) {
      this._db.close();
      this._closed = true;
    }
  }
}

module.exports = {
  WaveDB,
  WaveDBError,
  NotFoundError,
  InvalidPathError,
  IOError
};