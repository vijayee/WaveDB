'use strict';

// VectorLayer — Node.js wrapper for the WaveDB vector layer C API.
//
// Mirrors the GraphLayer JS wrapper. Two construction modes:
//   - VectorLayer.openSeparate(dbLocation, indexName, format, runtime)
//       Opens a DEDICATED vector database at dbLocation (no subtree).
//   - VectorLayer.open(indexName, db, format, runtime, subtree?)
//       Opens on an EXISTING WaveDB instance (shared key space). The
//       `db` argument is a WaveDB instance (we extract its underlying
//       database_t* via _getDbPtr). `subtree?` is an optional Subtree
//       instance — when provided, all keys land under that subtree
//       prefix.
//
// Config is split into Format (immutable after create) and Runtime
// (mutable via reconfigure). The C API has no slsh_bidirectional field
// (SLSH always scans bidirectionally) — this binding does NOT expose
// one. Trying to set it in runtimeObj is a no-op (the field is silently
// ignored, matching the C struct).
//
// Sync API is wired directly. Async variants (insert/search/delete)
// return Promises but currently just await the sync variant — the
// promise-based C async API needs a ThreadSafeFunction bridge which is
// a follow-up. JS callers can still benefit from non-blocking behaviour
// by using queueMicrotask / setImmediate wrappers if needed, but the
// underlying call runs synchronously on the main thread.

const native = require('../build/Release/wavedb.node');
const { VectorLayer: VectorLayerNative, VL_INDEX_FLAT, VL_INDEX_IVF, VL_INDEX_SLSH,
        VL_DIST_L2, VL_DIST_COSINE, VL_DIST_DOT } = native;

// Enum constants exposed for ergonomics.
const IndexType = {
  FLAT: VL_INDEX_FLAT,
  IVF:  VL_INDEX_IVF,
  SLSH: VL_INDEX_SLSH,
};
const Distance = {
  L2:     VL_DIST_L2,
  COSINE: VL_DIST_COSINE,
  DOT:    VL_DIST_DOT,
};

class VectorLayer {
  /**
   * Open a DEDICATED vector database at dbLocation.
   *
   * @param {string} dbLocation - Filesystem path for the vector db
   * @param {string} indexName - Index name (namespaces the layer's keys)
   * @param {object} format - Format config (immutable after create)
   * @param {object} runtime - Runtime config (mutable via reconfigure)
   * @returns {VectorLayer}
   */
  static openSeparate(dbLocation, indexName, format, runtime) {
    if (typeof dbLocation !== 'string' || !dbLocation) {
      throw new TypeError('dbLocation string required');
    }
    if (typeof indexName !== 'string' || !indexName) {
      throw new TypeError('indexName string required');
    }
    const fmt = _normalizeFormat(format);
    const rt = _normalizeRuntime(runtime);
    const nativeLayer = new VectorLayerNative(dbLocation, indexName, fmt, rt);
    const vl = new VectorLayer();
    vl._native = nativeLayer;
    vl._closed = false;
    return vl;
  }

  /**
   * Open on an EXISTING WaveDB instance (shared key space).
   *
   * @param {string} indexName - Index name
   * @param {WaveDB} db - A WaveDB instance (we extract its database_t*)
   * @param {object} format - Format config (immutable after create)
   * @param {object} runtime - Runtime config (mutable via reconfigure)
   * @param {Subtree} [subtree] - Optional Subtree (keys land under its prefix)
   * @returns {VectorLayer}
   */
  static open(indexName, db, format, runtime, subtree) {
    if (typeof indexName !== 'string' || !indexName) {
      throw new TypeError('indexName string required');
    }
    if (!db || typeof db._getDbPtr !== 'function') {
      throw new TypeError('db must be a WaveDB instance');
    }
    const dbPtr = db._getDbPtr();
    const fmt = _normalizeFormat(format);
    const rt = _normalizeRuntime(runtime);
    let subtreePtr = null;
    if (subtree && typeof subtree._getPtr === 'function') {
      subtreePtr = subtree._getPtr();
    }
    const args = [indexName, dbPtr, fmt, rt];
    if (subtreePtr !== null) args.push(subtreePtr);
    const nativeLayer = new VectorLayerNative(...args);
    const vl = new VectorLayer();
    vl._native = nativeLayer;
    vl._closed = false;
    return vl;
  }

  // --- Sync operations ---

  /**
   * Insert a vector with an optional metadata buffer.
   * @param {string} id
   * @param {Float32Array} vec
   * @param {Buffer} [metadata]
   */
  insertSync(id, vec, metadata) {
    _checkOpen(this);
    if (!(vec instanceof Float32Array)) {
      throw new TypeError('vec must be a Float32Array');
    }
    if (metadata !== undefined && metadata !== null && !Buffer.isBuffer(metadata)) {
      throw new TypeError('metadata must be a Buffer or null/undefined');
    }
    this._native.insertSync(id, vec, metadata || null);
  }

  /**
   * Insert a batch of vectors atomically.
   * @param {Array<{id: string, vec: Float32Array, metadata?: Buffer}>} items
   */
  insertBatchSync(items) {
    _checkOpen(this);
    if (!Array.isArray(items)) throw new TypeError('items must be an array');
    this._native.insertBatchSync(items);
  }

  /**
   * Search for the k nearest neighbors of `query`.
   * @param {Float32Array} query
   * @param {number} k
   * @returns {Array<{id: string, distance: number, metadata: Buffer|null}>}
   */
  searchSync(query, k) {
    _checkOpen(this);
    if (!(query instanceof Float32Array)) {
      throw new TypeError('query must be a Float32Array');
    }
    return this._native.searchSync(query, k);
  }

  /**
   * Delete a vector by id.
   * @param {string} id
   */
  deleteSync(id) {
    _checkOpen(this);
    this._native.deleteSync(id);
  }

  /**
   * Train the index (IVF: compute centroids; SLSH: generate projections;
   * FLAT: no-op).
   */
  train() {
    _checkOpen(this);
    this._native.train();
  }

  /**
   * Rewrite the index's per-vector entries from the stored vectors.
   */
  rebuild() {
    _checkOpen(this);
    this._native.rebuild();
  }

  /**
   * @returns {number} Number of vectors in the index.
   */
  count() {
    _checkOpen(this);
    return this._native.count();
  }

  /**
   * Reconfigure runtime-tunable params. Format tier is immutable.
   * @param {object} runtime
   */
  reconfigure(runtime) {
    _checkOpen(this);
    this._native.reconfigure(_normalizeRuntime(runtime));
  }

  /**
   * Close the layer and release native resources.
   */
  close() {
    if (this._native && !this._closed) {
      this._closed = true;
      this._native.close();
    }
  }

  get isClosed() {
    return this._closed;
  }

  // --- Async wrappers (deferred: call sync variants, wrap in Promise) ---
  //
  // These return Promises for API parity with the Graph layer, but the
  // underlying call currently runs synchronously on the main thread.
  // A true async bridge (ThreadSafeFunction + C promise_t) is a
  // follow-up — see vector_layer.cc header comment.

  /**
   * Async insert (currently sync-under-the-hood; returns a Promise).
   */
  insert(id, vec, metadata) {
    return Promise.resolve().then(() => {
      this.insertSync(id, vec, metadata);
    });
  }

  /**
   * Async batch insert (currently sync-under-the-hood; returns a Promise).
   */
  insertBatch(items) {
    return Promise.resolve().then(() => {
      this.insertBatchSync(items);
    });
  }

  /**
   * Async search (currently sync-under-the-hood; returns a Promise).
   */
  search(query, k) {
    return Promise.resolve().then(() => this.searchSync(query, k));
  }

  /**
   * Async delete (currently sync-under-the-hood; returns a Promise).
   */
  del(id) {
    return Promise.resolve().then(() => {
      this.deleteSync(id);
    });
  }
}

// --- helpers ---

function _checkOpen(vl) {
  if (vl._closed) throw new Error('VectorLayer is closed');
  if (!vl._native) throw new Error('VectorLayer not initialized');
}

// Normalize a user-supplied format object: ensure index_type / distance
// are integers, drop any unknown fields (e.g. slsh_bidirectional — the
// C API has no such field, so we silently ignore it rather than error,
// matching the C struct's "what you set is what you get" contract).
function _normalizeFormat(format) {
  if (!format || typeof format !== 'object') {
    throw new TypeError('format object required');
  }
  const out = {
    dim: format.dim | 0,
  };
  if (format.index_type !== undefined) {
    out.index_type = format.index_type | 0;
  }
  if (format.delimiter !== undefined) {
    out.delimiter = format.delimiter;
  }
  if (format.distance !== undefined) {
    out.distance = format.distance | 0;
  }
  if (format.ivf_n_clusters !== undefined) {
    out.ivf_n_clusters = format.ivf_n_clusters | 0;
  }
  if (format.slsh_lsh_tables !== undefined) {
    out.slsh_lsh_tables = format.slsh_lsh_tables | 0;
  }
  if (format.slsh_hash_bits !== undefined) {
    out.slsh_hash_bits = format.slsh_hash_bits | 0;
  }
  if (format.slsh_bucket_width !== undefined) {
    out.slsh_bucket_width = +format.slsh_bucket_width;
  }
  return out;
}

// Normalize a user-supplied runtime object. Unknown fields (e.g.
// slsh_bidirectional) are silently dropped.
function _normalizeRuntime(runtime) {
  if (!runtime || typeof runtime !== 'object') {
    throw new TypeError('runtime object required');
  }
  const out = {};
  if (runtime.top_k !== undefined)           out.top_k           = runtime.top_k | 0;
  if (runtime.sync_only !== undefined)       out.sync_only       = runtime.sync_only | 0;
  if (runtime.ivf_nprobe !== undefined)      out.ivf_nprobe      = runtime.ivf_nprobe | 0;
  if (runtime.ivf_flat_until !== undefined)  out.ivf_flat_until  = runtime.ivf_flat_until | 0;
  if (runtime.slsh_scan_radius !== undefined) out.slsh_scan_radius = runtime.slsh_scan_radius | 0;
  return out;
}

module.exports = {
  VectorLayer,
  IndexType,
  Distance,
  // Re-export raw enum values too for callers who want them.
  VL_INDEX_FLAT,
  VL_INDEX_IVF,
  VL_INDEX_SLSH,
  VL_DIST_L2,
  VL_DIST_COSINE,
  VL_DIST_DOT,
};