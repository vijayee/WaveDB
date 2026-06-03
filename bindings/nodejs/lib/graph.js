'use strict';

const { GraphLayer: GraphLayerNative } = require('../build/Release/graph.node');

let _defaultGraph = null;

class Query {
  constructor(layer) {
    this._layer = layer !== undefined ? layer : _defaultGraph;
    this._steps = [];
  }

  V(id)          { this._steps.push(`V("${id}")`); return this; }
  Out(pred)      { this._steps.push(`Out("${pred}")`); return this; }
  In(pred)       { this._steps.push(`In("${pred}")`); return this; }
  Has(pred, val) { this._steps.push(`Has("${pred}","${val}")`); return this; }
  HasGt(pred, val)  { this._steps.push(`Has("${pred}",>, "${val}")`); return this; }
  HasGte(pred, val) { this._steps.push(`Has("${pred}",>=, "${val}")`); return this; }
  HasLt(pred, val)  { this._steps.push(`Has("${pred}",<, "${val}")`); return this; }
  HasLte(pred, val) { this._steps.push(`Has("${pred}",<=, "${val}")`); return this; }
  And(sub)       { this._steps.push(`And(${sub._toDSL()})`); return this; }
  Or(sub)        { this._steps.push(`Or(${sub._toDSL()})`); return this; }
  Not(sub)       { this._steps.push(`Not(${sub._toDSL()})`); return this; }
  Difference(sub) { this._steps.push(`Difference(${sub._toDSL()})`); return this; }
  Follow(name)   { this._steps.push(`Follow("${name}")`); return this; }
  Limit(n)       { this._steps.push(`Limit(${n})`); return this; }
  /** Appends Count() — use with graph.count(), not .All() */
  Count()        { this._steps.push(`Count()`); return this; }

  _toDSL() {
    return `g.${this._steps.join('.')}`;
  }

  All() {
    if (!this._layer) {
      throw new Error('Query is not bound to a GraphLayer. ' +
        'Create a GraphLayer or use graph.exec(query._toDSL())');
    }
    return this._layer.exec(this._toDSL());
  }

  toString() {
    return this._toDSL();
  }
}

// g is a traversal source object (Gremlin-style).
// g.V(), g.Has() etc. create new Query objects bound to the default graph.
const QUERY_METHODS = new Set([
  'V', 'Out', 'In', 'Has', 'HasGt', 'HasGte', 'HasLt', 'HasLte',
  'And', 'Or', 'Not', 'Difference', 'Follow', 'Limit', 'Count', 'All', 'toString'
]);
const g = new Proxy({}, {
  get(_target, prop) {
    if (prop === Symbol.toPrimitive || prop === 'inspect' || prop === 'then') {
      return undefined;
    }
    if (!QUERY_METHODS.has(prop)) return undefined;
    return function (...args) {
      const q = new Query(_defaultGraph);
      return q[prop](...args);
    };
  }
});

function setDefaultGraph(layer) {
  _defaultGraph = layer;
}

class GraphLayer {
  constructor(path, options = {}) {
    // Pass subtree pointer through to native if provided
    const nativeOptions = { ...options };
    if (options.subtree && options.subtree._st) {
      nativeOptions._subtreePtr = options.subtree._st._getPtr();
    }
    delete nativeOptions.subtree;
    this._layer = new GraphLayerNative(path, nativeOptions);
    this._closed = false;
    if (!_defaultGraph) setDefaultGraph(this);
  }

  // Async triple operations (return Promises)
  insert(s, p, o) {
    if (this._closed) throw new Error('GraphLayer is closed');
    return this._layer.insert(s, p, o);
  }

  del(s, p, o) {
    if (this._closed) throw new Error('GraphLayer is closed');
    return this._layer.del(s, p, o);
  }

  // Async query (returns Promise<string[]>)
  query(dsl) {
    if (this._closed) throw new Error('GraphLayer is closed');
    if (dsl instanceof Query) dsl = dsl._toDSL();
    return this._layer.query(dsl);
  }

  // Sync operations
  exec(dsl) {
    if (this._closed) throw new Error('GraphLayer is closed');
    if (dsl instanceof Query) dsl = dsl._toDSL();
    return this._layer.exec(dsl);
  }

  count(dsl) {
    if (this._closed) throw new Error('GraphLayer is closed');
    if (dsl instanceof Query) dsl = dsl._toDSL();
    return this._layer.count(dsl);
  }

  insertSync(s, p, o) {
    if (this._closed) throw new Error('GraphLayer is closed');
    this._layer.insertSync(s, p, o);
  }

  deleteSync(s, p, o) {
    if (this._closed) throw new Error('GraphLayer is closed');
    this._layer.deleteSync(s, p, o);
  }

  parseSchema(sdl) {
    if (this._closed) throw new Error('GraphLayer is closed');
    this._layer.parseSchema(sdl);
  }

  defineMorphism(name, dsl) {
    if (this._closed) throw new Error('GraphLayer is closed');
    this._layer.defineMorphism(name, dsl);
  }

  close() {
    if (this._layer && !this._closed) {
      if (_defaultGraph === this) _defaultGraph = null;
      this._closed = true;
      this._layer.close();
    }
  }

  get isClosed() {
    return this._closed;
  }
}

module.exports = {
  GraphLayer,
  Query,
  g,
  setDefaultGraph,
};