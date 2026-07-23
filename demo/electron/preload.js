'use strict';

const { contextBridge, ipcRenderer } = require('electron');
const path = require('path');
const os = require('os');

function tempPath(requested) {
  if (requested !== ':memory:') return requested;
  return path.join(
    os.tmpdir(),
    `wavedb-demo-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`
  );
}

function send(channel, ...args) {
  return ipcRenderer.sendSync(channel, ...args);
}

function invoke(channel, ...args) {
  const res = send(channel, ...args);
  if (!res || res.ok) return res ? res.value : undefined;
  const err = new Error(res.error);
  err.name = res.name || 'Error';
  throw err;
}

// ─────────────────────────────────────────────────────────────
// WaveDB proxy
// ─────────────────────────────────────────────────────────────

class WaveDB {
  constructor(path, options = {}) {
    this._handle = invoke('WAVEDB_OPEN', tempPath(path), options);
  }
  putSync(key, value) { invoke('WAVEDB_PUT_SYNC', this._handle, key, value); }
  getSync(key) { return invoke('WAVEDB_GET_SYNC', this._handle, key); }
  delSync(key) { invoke('WAVEDB_DEL_SYNC', this._handle, key); }
  batchSync(ops) { invoke('WAVEDB_BATCH_SYNC', this._handle, ops); }
  putObject(key, obj) { invoke('WAVEDB_PUT_OBJECT', this._handle, key, obj); }
  getObjectSync(key) { return invoke('WAVEDB_GET_OBJECT_SYNC', this._handle, key); }
  close() { invoke('WAVEDB_CLOSE', this._handle); }
}

// ─────────────────────────────────────────────────────────────
// GraphQL proxy
// ─────────────────────────────────────────────────────────────

class GraphQLLayer {
  constructor(path, options = {}) {
    this._handle = invoke('GQL_OPEN', path, options);
  }
  parseSchema(sdl) { invoke('GQL_PARSE_SCHEMA', this._handle, sdl); }
  mutateSync(mutation) { return invoke('GQL_MUTATE_SYNC', this._handle, mutation); }
  querySync(query) { return invoke('GQL_QUERY_SYNC', this._handle, query); }
  close() { invoke('GQL_CLOSE', this._handle); }
}

// ─────────────────────────────────────────────────────────────
// Graph proxy + g DSL builder
// ─────────────────────────────────────────────────────────────

let defaultGraphHandle = null;

class GraphLayer {
  constructor(path, options = {}) {
    this._handle = invoke('GRAPH_OPEN', tempPath(path), options);
    defaultGraphHandle = this._handle;
  }
  insertSync(s, p, o) { invoke('GRAPH_INSERT_SYNC', this._handle, s, p, o); }
  deleteSync(s, p, o) { invoke('GRAPH_DELETE_SYNC', this._handle, s, p, o); }
  exec(dsl) { return invoke('GRAPH_EXEC', this._handle, dsl instanceof Query ? dsl._toDSL() : dsl); }
  count(dsl) { return invoke('GRAPH_COUNT', this._handle, dsl instanceof Query ? dsl._toDSL() : dsl); }
  close() { invoke('GRAPH_CLOSE', this._handle); }
}

class Query {
  constructor() { this._steps = []; }
  V(id) {
    if (id !== undefined) this._steps.push(`V("${id}")`);
    else this._steps.push('V()');
    return this;
  }
  Out(pred) { this._steps.push(`Out("${pred}")`); return this; }
  In(pred) { this._steps.push(`In("${pred}")`); return this; }
  Has(pred, val) { this._steps.push(`Has("${pred}","${val}")`); return this; }
  Limit(n) { this._steps.push(`Limit(${n})`); return this; }
  Count() { this._steps.push(`Count()`); return this; }
  All() { this._steps.push(`All()`); return this; }
  _toDSL() { return `g.${this._steps.join('.')}`; }
  toString() { return this._toDSL(); }
}

const QUERY_METHODS = new Set(['V','Out','In','Has','Limit','Count','All','toString']);
const g = new Proxy({}, {
  get(_target, prop) {
    if (prop === Symbol.toPrimitive || prop === 'inspect' || prop === 'then') return undefined;
    if (!QUERY_METHODS.has(prop)) return undefined;
    return function (...args) {
      const q = new Query();
      return q[prop](...args);
    };
  }
});

function execDefaultGraph(dsl) {
  if (!defaultGraphHandle) throw new Error('No GraphLayer has been created yet');
  return invoke('GRAPH_EXEC', defaultGraphHandle, dsl instanceof Query ? dsl._toDSL() : dsl);
}

function countDefaultGraph(dsl) {
  if (!defaultGraphHandle) throw new Error('No GraphLayer has been created yet');
  return invoke('GRAPH_COUNT', defaultGraphHandle, dsl instanceof Query ? dsl._toDSL() : dsl);
}

// Patch Query.All/Count so g-chains use the default graph automatically.
Query.prototype.All = function () {
  this._steps.push('All()');
  return execDefaultGraph(this);
};
Query.prototype.Count = function () {
  this._steps.push('Count()');
  return countDefaultGraph(this);
};

// ─────────────────────────────────────────────────────────────
// Vector proxy
// ─────────────────────────────────────────────────────────────

class VectorLayer {
  static IndexType = { FLAT: 0, IVF: 1, SLSH: 2 };
  static Distance = { L2: 0, COSINE: 1, DOT: 2 };

  static openSeparate(dbLocation, indexName, format, runtime) {
    const handle = invoke('VECTOR_OPEN_SEPARATE', tempPath(dbLocation), indexName, format, runtime);
    return new VectorLayer(handle);
  }

  static open(indexName, db, format, runtime, subtree) {
    const handle = invoke('VECTOR_OPEN', indexName, db._handle, format, runtime, subtree);
    return new VectorLayer(handle);
  }

  constructor(handle) {
    if (typeof handle !== 'number') throw new TypeError('Use openSeparate/open');
    this._handle = handle;
  }

  insertSync(id, vec, metadata) {
    const arr = vec instanceof Float32Array ? Array.from(vec) : vec;
    invoke('VECTOR_INSERT_SYNC', this._handle, id, arr, metadata || null);
  }

  deleteSync(id) { invoke('VECTOR_DELETE_SYNC', this._handle, id); }

  searchSync(query, k) {
    const arr = query instanceof Float32Array ? Array.from(query) : query;
    return invoke('VECTOR_SEARCH_SYNC', this._handle, arr, k);
  }

  count() { return invoke('VECTOR_COUNT', this._handle); }
  train() { invoke('VECTOR_TRAIN', this._handle); }
  rebuild() { invoke('VECTOR_REBUILD', this._handle); }
  close() { invoke('VECTOR_CLOSE', this._handle); }
}

contextBridge.exposeInMainWorld('electronAPI', {
  isElectron: true,
  WaveDB,
  GraphQLLayer,
  GraphLayer,
  g,
  VectorLayer
});
