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
  And(sub)       { this._steps.push(`And(${sub._toDSL()})`); return this; }
  Or(sub)        { this._steps.push(`Or(${sub._toDSL()})`); return this; }
  Limit(n)       { this._steps.push(`Limit(${n})`); return this; }
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
  'V', 'Out', 'In', 'Has', 'And', 'Or', 'Limit', 'Count', 'All', 'toString'
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
    this._layer = new GraphLayerNative(path, options);
    this._closed = false;
    if (!_defaultGraph) setDefaultGraph(this);
  }

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
