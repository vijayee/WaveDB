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

function requireShim(id) {
  if (id === 'path') return path;
  if (id === 'os') return os;
  throw new Error(`Module not available in renderer: ${id}`);
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

function createWaveDB(path, options = {}) {
  const handle = invoke('WAVEDB_OPEN', tempPath(path), options);
  return {
    putSync: (key, value) => invoke('WAVEDB_PUT_SYNC', handle, key, value),
    getSync: (key) => invoke('WAVEDB_GET_SYNC', handle, key),
    delSync: (key) => invoke('WAVEDB_DEL_SYNC', handle, key),
    batchSync: (ops) => invoke('WAVEDB_BATCH_SYNC', handle, ops),
    putObjectSync: (key, obj) => invoke('WAVEDB_PUT_OBJECT_SYNC', handle, key, obj),
    getObjectSync: (key) => invoke('WAVEDB_GET_OBJECT_SYNC', handle, key),
    close: () => invoke('WAVEDB_CLOSE', handle)
  };
}

// ─────────────────────────────────────────────────────────────
// GraphQL proxy
// ─────────────────────────────────────────────────────────────

function createGraphQLLayer(path, options = {}) {
  const handle = invoke('GQL_OPEN', path, options);
  return {
    parseSchema: (sdl) => invoke('GQL_PARSE_SCHEMA', handle, sdl),
    mutateSync: (mutation) => invoke('GQL_MUTATE_SYNC', handle, mutation),
    querySync: (query) => invoke('GQL_QUERY_SYNC', handle, query),
    close: () => invoke('GQL_CLOSE', handle)
  };
}

// ─────────────────────────────────────────────────────────────
// Graph proxy + g DSL builder
// ─────────────────────────────────────────────────────────────

let defaultGraphHandle = null;

function createGraphLayer(path, options = {}) {
  const handle = invoke('GRAPH_OPEN', tempPath(path), options);
  defaultGraphHandle = handle;
  return {
    insertSync: (s, p, o) => invoke('GRAPH_INSERT_SYNC', handle, s, p, o),
    deleteSync: (s, p, o) => invoke('GRAPH_DELETE_SYNC', handle, s, p, o),
    exec: (dsl) => {
      const normalized = normalizeDSL(dsl);
      if (typeof normalized !== 'string') return normalized;
      return invoke('GRAPH_EXEC', handle, normalized);
    },
    count: (dsl) => {
      const normalized = normalizeDSL(dsl);
      if (typeof normalized !== 'string') return normalized;
      return invoke('GRAPH_COUNT', handle, normalized);
    },
    close: () => invoke('GRAPH_CLOSE', handle)
  };
}

function normalizeDSL(dsl) {
  if (typeof dsl === 'object' && dsl !== null && typeof dsl._toDSL === 'function') {
    return dsl._toDSL();
  }
  if (typeof dsl === 'string') return dsl;
  // Already-computed query result passed through unchanged.
  return dsl;
}

function createQuery() {
  const q = { _steps: [] };

  q.V = (id) => {
    q._steps.push(id !== undefined ? `V("${id}")` : 'V()');
    return q;
  };
  q.Out = (pred) => { q._steps.push(`Out("${pred}")`); return q; };
  q.In = (pred) => { q._steps.push(`In("${pred}")`); return q; };
  q.Has = (pred, val) => { q._steps.push(`Has("${pred}","${val}")`); return q; };
  q.Limit = (n) => { q._steps.push(`Limit(${n})`); return q; };
  q.Count = () => { q._steps.push('Count()'); return countDefaultGraph(q._toDSL()); };
  q.All = () => { q._steps.push('All()'); return execDefaultGraph(q._toDSL()); };
  q._toDSL = () => `g.${q._steps.join('.')}`;
  q.toString = q._toDSL;

  return q;
}

const g = {
  V(...args) { return createQuery().V(...args); },
  Out(...args) { return createQuery().Out(...args); },
  In(...args) { return createQuery().In(...args); },
  Has(...args) { return createQuery().Has(...args); },
  Limit(...args) { return createQuery().Limit(...args); },
  Count(...args) { return createQuery().Count(...args); },
  All(...args) { return createQuery().All(...args); },
  toString() { return 'g'; }
};

function execDefaultGraph(dsl) {
  if (!defaultGraphHandle) throw new Error('No GraphLayer has been created yet');
  const normalized = normalizeDSL(dsl);
  if (typeof normalized !== 'string') return normalized;
  return invoke('GRAPH_EXEC', defaultGraphHandle, normalized);
}

function countDefaultGraph(dsl) {
  if (!defaultGraphHandle) throw new Error('No GraphLayer has been created yet');
  const normalized = normalizeDSL(dsl);
  if (typeof normalized !== 'string') return normalized;
  return invoke('GRAPH_COUNT', defaultGraphHandle, normalized);
}

// ─────────────────────────────────────────────────────────────
// Vector proxy
// ─────────────────────────────────────────────────────────────

function createVectorLayer(handle) {
  return {
    insertSync: (id, vec, metadata) => {
      const arr = vec instanceof Float32Array ? Array.from(vec) : vec;
      invoke('VECTOR_INSERT_SYNC', handle, id, arr, metadata || null);
    },
    deleteSync: (id) => invoke('VECTOR_DELETE_SYNC', handle, id),
    searchSync: (query, k) => {
      const arr = query instanceof Float32Array ? Array.from(query) : query;
      return invoke('VECTOR_SEARCH_SYNC', handle, arr, k);
    },
    count: () => invoke('VECTOR_COUNT', handle),
    train: () => invoke('VECTOR_TRAIN', handle),
    rebuild: () => invoke('VECTOR_REBUILD', handle),
    close: () => invoke('VECTOR_CLOSE', handle)
  };
}

function openVectorLayerSeparate(dbLocation, indexName, format, runtime) {
  const handle = invoke('VECTOR_OPEN_SEPARATE', tempPath(dbLocation), indexName, format, runtime);
  return createVectorLayer(handle);
}

const vectorLayerApi = {
  openSeparate: openVectorLayerSeparate
};

// contextBridge can expose functions, but objects returned from exposed
// functions only preserve *own* function properties. Returning plain objects
// with methods as own properties lets the renderer call them directly while
// keeping the same user-facing API (new WaveDB(...), VectorLayer.openSeparate).
contextBridge.exposeInMainWorld('electronAPI', {
  isElectron: true,
  WaveDB: createWaveDB,
  GraphQLLayer: createGraphQLLayer,
  GraphLayer: createGraphLayer,
  g,
  VectorLayer: vectorLayerApi,
  require: requireShim,
  path,
  os
});
