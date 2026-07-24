'use strict';

const { app, BrowserWindow, ipcMain } = require('electron');
const path = require('path');

const { WaveDB } = require('@vijayee/wavedb');
const { GraphQLLayer } = require('@vijayee/wavedb/graphql');
const { GraphLayer, g, setDefaultGraph } = require('@vijayee/wavedb/graph');
const { VectorLayer } = require('@vijayee/wavedb/vector_layer');

const HANDLES = {
  waveDb: new Map(),
  graphQl: new Map(),
  graph: new Map(),
  vector: new Map(),
};

let nextHandle = 1;

function allocHandle(namespace, obj) {
  const id = nextHandle++;
  HANDLES[namespace].set(id, obj);
  return id;
}

function getHandle(namespace, id) {
  const obj = HANDLES[namespace].get(id);
  if (!obj) throw new Error(`${namespace} handle ${id} not found or already closed`);
  return obj;
}

function releaseHandle(namespace, id) {
  HANDLES[namespace].delete(id);
}

function respond(fn) {
  try {
    const result = fn();
    return { ok: true, value: result };
  } catch (err) {
    return { ok: false, error: err.message || String(err), name: err.name };
  }
}

function unpackError(res) {
  if (!res || res.ok) return;
  const e = new Error(res.error);
  e.name = res.name || 'Error';
  throw e;
}

// ─────────────────────────────────────────────────────────────
// WaveDB
// ─────────────────────────────────────────────────────────────

ipcMain.on('WAVEDB_OPEN', (event, path, options) => {
  event.returnValue = respond(() => allocHandle('waveDb', new WaveDB(path, options || {})));
});

ipcMain.on('WAVEDB_PUT_SYNC', (event, handle, key, value) => {
  event.returnValue = respond(() => {
    getHandle('waveDb', handle).putSync(key, value);
    return null;
  });
});

ipcMain.on('WAVEDB_GET_SYNC', (event, handle, key) => {
  event.returnValue = respond(() => getHandle('waveDb', handle).getSync(key));
});

ipcMain.on('WAVEDB_DEL_SYNC', (event, handle, key) => {
  event.returnValue = respond(() => {
    getHandle('waveDb', handle).delSync(key);
    return null;
  });
});

ipcMain.on('WAVEDB_BATCH_SYNC', (event, handle, ops) => {
  event.returnValue = respond(() => {
    getHandle('waveDb', handle).batchSync(ops);
    return null;
  });
});

ipcMain.on('WAVEDB_PUT_OBJECT', (event, handle, key, obj) => {
  event.returnValue = respond(() => {
    getHandle('waveDb', handle).putObject(key, obj);
    return null;
  });
});

ipcMain.on('WAVEDB_GET_OBJECT_SYNC', (event, handle, key) => {
  event.returnValue = respond(() => getHandle('waveDb', handle).getObjectSync(key));
});

ipcMain.on('WAVEDB_CLOSE', (event, handle) => {
  event.returnValue = respond(() => {
    const db = getHandle('waveDb', handle);
    db.close();
    releaseHandle('waveDb', handle);
    return null;
  });
});

// ─────────────────────────────────────────────────────────────
// GraphQL
// ─────────────────────────────────────────────────────────────

ipcMain.on('GQL_OPEN', (event, path, options) => {
  event.returnValue = respond(() => allocHandle('graphQl', new GraphQLLayer(path, options || {})));
});

ipcMain.on('GQL_PARSE_SCHEMA', (event, handle, sdl) => {
  event.returnValue = respond(() => {
    getHandle('graphQl', handle).parseSchema(sdl);
    return null;
  });
});

ipcMain.on('GQL_MUTATE_SYNC', (event, handle, mutation) => {
  event.returnValue = respond(() => getHandle('graphQl', handle).mutateSync(mutation));
});

ipcMain.on('GQL_QUERY_SYNC', (event, handle, query) => {
  event.returnValue = respond(() => getHandle('graphQl', handle).querySync(query));
});

ipcMain.on('GQL_CLOSE', (event, handle) => {
  event.returnValue = respond(() => {
    getHandle('graphQl', handle).close();
    releaseHandle('graphQl', handle);
    return null;
  });
});

// ─────────────────────────────────────────────────────────────
// Graph
// ─────────────────────────────────────────────────────────────

ipcMain.on('GRAPH_OPEN', (event, path, options) => {
  event.returnValue = respond(() => {
    const layer = new GraphLayer(path, options || {});
    setDefaultGraph(layer);
    return allocHandle('graph', layer);
  });
});

ipcMain.on('GRAPH_INSERT_SYNC', (event, handle, s, p, o) => {
  event.returnValue = respond(() => {
    getHandle('graph', handle).insertSync(s, p, o);
    return null;
  });
});

ipcMain.on('GRAPH_DELETE_SYNC', (event, handle, s, p, o) => {
  event.returnValue = respond(() => {
    getHandle('graph', handle).deleteSync(s, p, o);
    return null;
  });
});

ipcMain.on('GRAPH_EXEC', (event, handle, dsl) => {
  event.returnValue = respond(() => getHandle('graph', handle).exec(dsl));
});

ipcMain.on('GRAPH_COUNT', (event, handle, dsl) => {
  event.returnValue = respond(() => getHandle('graph', handle).count(dsl));
});

ipcMain.on('GRAPH_CLOSE', (event, handle) => {
  event.returnValue = respond(() => {
    getHandle('graph', handle).close();
    releaseHandle('graph', handle);
    return null;
  });
});

// ─────────────────────────────────────────────────────────────
// Vector
// ─────────────────────────────────────────────────────────────

ipcMain.on('VECTOR_OPEN_SEPARATE', (event, dbLocation, indexName, format, runtime) => {
  event.returnValue = respond(() =>
    allocHandle('vector', VectorLayer.openSeparate(dbLocation, indexName, format, runtime))
  );
});

ipcMain.on('VECTOR_OPEN', (event, indexName, dbHandle, format, runtime) => {
  event.returnValue = respond(() => {
    const db = getHandle('waveDb', dbHandle);
    return allocHandle('vector', VectorLayer.open(indexName, db, format, runtime));
  });
});

ipcMain.on('VECTOR_INSERT_SYNC', (event, handle, id, vec, metadata) => {
  event.returnValue = respond(() => {
    getHandle('vector', handle).insertSync(id, new Float32Array(vec), metadata || null);
    return null;
  });
});

ipcMain.on('VECTOR_DELETE_SYNC', (event, handle, id) => {
  event.returnValue = respond(() => {
    getHandle('vector', handle).deleteSync(id);
    return null;
  });
});

ipcMain.on('VECTOR_SEARCH_SYNC', (event, handle, query, k) => {
  event.returnValue = respond(() => getHandle('vector', handle).searchSync(new Float32Array(query), k));
});

ipcMain.on('VECTOR_COUNT', (event, handle) => {
  event.returnValue = respond(() => getHandle('vector', handle).count());
});

ipcMain.on('VECTOR_TRAIN', (event, handle) => {
  event.returnValue = respond(() => {
    getHandle('vector', handle).train();
    return null;
  });
});

ipcMain.on('VECTOR_REBUILD', (event, handle) => {
  event.returnValue = respond(() => {
    getHandle('vector', handle).rebuild();
    return null;
  });
});

ipcMain.on('VECTOR_CLOSE', (event, handle) => {
  event.returnValue = respond(() => {
    getHandle('vector', handle).close();
    releaseHandle('vector', handle);
    return null;
  });
});

// ─────────────────────────────────────────────────────────────
// Window
// ─────────────────────────────────────────────────────────────

function createWindow() {
  const win = new BrowserWindow({
    width: 1280,
    height: 800,
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true
    }
  });

  win.loadFile(path.join(__dirname, '..', 'index.html'));

  // Open DevTools with Ctrl+Shift+I or F12 is handled by default;
  // uncomment the next line to open them automatically.
  // win.webContents.openDevTools();
}

app.whenReady().then(createWindow);

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') app.quit();
});

app.on('activate', () => {
  if (BrowserWindow.getAllWindows().length === 0) createWindow();
});

// Cleanup any remaining native handles before quit.
app.on('before-quit', () => {
  for (const [, db] of HANDLES.waveDb) try { db.close(); } catch (e) {}
  for (const [, gql] of HANDLES.graphQl) try { gql.close(); } catch (e) {}
  for (const [, graph] of HANDLES.graph) try { graph.close(); } catch (e) {}
  for (const [, vec] of HANDLES.vector) try { vec.close(); } catch (e) {}
});
