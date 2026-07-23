/**
 * WaveDB HTML5 Demo — in-browser mock APIs and slide runner.
 * These mocks approximate the Node.js binding signatures so the
 * live code examples run without a native build or backend.
 */

(function () {
  'use strict';

  // ─────────────────────────────────────────────────────────────
  // Helpers
  // ─────────────────────────────────────────────────────────────

  function normalizeKey(key, delimiter = '/') {
    if (Array.isArray(key)) return key.join(delimiter);
    return String(key);
  }

  function formatArg(arg) {
    if (arg === undefined) return 'undefined';
    if (arg === null) return 'null';
    if (typeof arg === 'string') return arg;
    if (typeof arg === 'number' || typeof arg === 'boolean') return String(arg);
    if (arg instanceof Float32Array) return `Float32Array([${Array.from(arg).map(v => v.toFixed(3)).join(', ')}])`;
    try {
      return JSON.stringify(arg, null, 2);
    } catch (e) {
      return String(arg);
    }
  }

  function parseArgs(str) {
    const args = {};
    const re = /(\w+)\s*:\s*(?:"([^"]*)"|(\d+))/g;
    let m;
    while ((m = re.exec(str)) !== null) {
      const key = m[1];
      const val = m[2] !== undefined ? m[2] : Number(m[3]);
      args[key] = val;
    }
    return args;
  }

  // ─────────────────────────────────────────────────────────────
  // Mock WaveDB
  // ─────────────────────────────────────────────────────────────

  class WaveDB {
    constructor(path, options = {}) {
      if (path === undefined || path === null || (typeof path !== 'string')) {
        throw new Error('Database path is required');
      }
      this._path = path;
      this._delimiter = options.delimiter || '/';
      this._store = new Map();
      this._closed = false;
    }

    _checkOpen() {
      if (this._closed) throw new Error('Database is closed');
    }

    _nk(key) { return normalizeKey(key, this._delimiter); }

    putSync(key, value) {
      this._checkOpen();
      this._store.set(this._nk(key), value);
    }

    getSync(key) {
      this._checkOpen();
      return this._store.has(this._nk(key)) ? this._store.get(this._nk(key)) : null;
    }

    delSync(key) {
      this._checkOpen();
      this._store.delete(this._nk(key));
    }

    batchSync(ops) {
      this._checkOpen();
      for (const op of ops) {
        const k = this._nk(op.key);
        if (op.type === 'put') this._store.set(k, op.value);
        else if (op.type === 'del') this._store.delete(k);
      }
    }

    putObject(key, obj) {
      this._checkOpen();
      this._store.set(this._nk(key), { __wave_object: true, value: obj });
    }

    getObjectSync(key) {
      this._checkOpen();
      const k = this._nk(key);
      if (!this._store.has(k)) return null;
      const v = this._store.get(k);
      if (v && typeof v === 'object' && v.__wave_object) return v.value;
      try {
        return JSON.parse(v);
      } catch (e) {
        return v;
      }
    }

    close() { this._closed = true; }
  }

  // ─────────────────────────────────────────────────────────────
  // Mock GraphQL Layer
  // ─────────────────────────────────────────────────────────────

  class GraphQLLayer {
    constructor(path, options = {}) {
      this._schema = null;
      this._last = null;
      this._closed = false;
    }

    _checkOpen() { if (this._closed) throw new Error('GraphQL layer is closed'); }

    parseSchema(sdl) {
      this._checkOpen();
      this._schema = sdl;
    }

    mutateSync(mutation) {
      this._checkOpen();
      const call = mutation.match(/(\w+)\s*\(([^)]*)\)/);
      if (!call) return { success: false, errors: ['No mutation call found'] };
      const [, name, argStr] = call;
      const args = parseArgs(argStr);
      this._last = args;
      return { success: true, data: { [name]: args } };
    }

    querySync(query) {
      this._checkOpen();
      const call = query.match(/(\w+)\s*\(([^)]*)\)/);
      if (!call) {
        if (this._last) return { success: true, data: { person: this._last } };
        return { success: false, errors: ['No query call found'] };
      }
      const [, , argStr] = call;
      const args = parseArgs(argStr);
      const name = args.name || (this._last && this._last.name);
      if (this._last && this._last.name === name) {
        return { success: true, data: { person: this._last } };
      }
      return { success: true, data: { person: null } };
    }

    close() { this._closed = true; }
  }

  // ─────────────────────────────────────────────────────────────
  // Mock Graph Layer + g traversal DSL
  // ─────────────────────────────────────────────────────────────

  class Query {
    constructor(layer) {
      this._layer = layer;
      this._steps = [];
    }
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

  const QUERY_METHODS = new Set([
    'V', 'Out', 'In', 'Has', 'Limit', 'Count', 'All', 'toString'
  ]);

  const g = new Proxy({}, {
    get(_target, prop) {
      if (prop === Symbol.toPrimitive || prop === 'inspect' || prop === 'then') return undefined;
      if (!QUERY_METHODS.has(prop)) return undefined;
      return function (...args) {
        const q = new Query(null);
        return q[prop](...args);
      };
    }
  });

  class GraphLayer {
    constructor(path, options = {}) {
      this._triples = [];
      this._closed = false;
    }

    _checkOpen() { if (this._closed) throw new Error('GraphLayer is closed'); }

    insertSync(s, p, o) {
      this._checkOpen();
      this._triples.push([s, p, o]);
    }

    deleteSync(s, p, o) {
      this._checkOpen();
      this._triples = this._triples.filter(t => !(t[0] === s && t[1] === p && t[2] === o));
    }

    _runDSL(dsl) {
      if (dsl instanceof Query) dsl = dsl._toDSL();
      const steps = dsl.replace(/^g\./, '').split('.');
      let subject = null;
      let has = [];
      let wantsCount = false;

      for (const step of steps) {
        if (step.startsWith('V(')) {
          const id = step.match(/V\("([^"]*)"\)/)?.[1];
          if (id !== undefined) subject = id;
        } else if (step.startsWith('Has(')) {
          const m = step.match(/Has\("([^"]*)","([^"]*)"\)/);
          if (m) has.push({ pred: m[1], val: m[2] });
        } else if (step === 'Count()') {
          wantsCount = true;
        }
      }

      const matches = new Set();
      for (const [s, p, o] of this._triples) {
        if (subject !== null && s !== subject) continue;
        if (has.length && !has.every(h => {
          if (h.pred === p && h.val === o) return true;
          // value lookup: predicate 'name' with value 'Alice' matches subject's triple
          return this._triples.some(t => t[0] === s && t[1] === h.pred && t[2] === h.val);
        })) continue;
        matches.add(s);
      }
      const arr = Array.from(matches);
      return wantsCount ? arr.length : arr;
    }

    exec(dsl) { this._checkOpen(); return this._runDSL(dsl); }
    count(dsl) {
      this._checkOpen();
      const r = this._runDSL(dsl);
      return Array.isArray(r) ? r.length : r;
    }
    close() { this._closed = true; }
  }

  // ─────────────────────────────────────────────────────────────
  // Mock Vector Layer + deterministic embeddings
  // ─────────────────────────────────────────────────────────────

  const CONCEPTS = {
    party: [1.0, 0.9, 0, 0, 0, 0, 0, 0],
    dance: [0.95, 0.85, 0, 0, 0, 0, 0, 0],
    energetic: [0.9, 0.7, 0.1, 0, 0, 0, 0, 0],
    upbeat: [0.9, 0.8, 0, 0, 0, 0, 0, 0],
    intense: [0.85, 0.5, 0.2, 0, 0, 0, 0, 0],
    hype: [0.9, 0.6, 0.1, 0, 0, 0, 0, 0],
    workout: [0.85, 0.55, 0, 0, 0, 0, 0.9, 0],
    chill: [0, 0, 1.0, 0.9, 0, 0, 0, 0],
    mellow: [0, 0, 0.95, 0.85, 0, 0, 0, 0],
    calm: [0, 0, 0.9, 0.95, 0, 0, 0, 0],
    lofi: [0, 0, 0.85, 0.8, 0, 0, 0, 0],
    beats: [0.1, 0, 0.8, 0.7, 0, 0, 0, 0],
    sad: [0, 0, 0, 0, 1.0, 0.9, 0, 0],
    heartbreak: [0, 0, 0, 0, 0.95, 0.85, 0, 0],
    acoustic: [0, 0, 0, 0, 0.85, 0.9, 0, 0],
    ballad: [0, 0, 0, 0, 0.9, 0.8, 0, 0],
    track: [0.5, 0.5, 0.5, 0.5, 0, 0, 0, 0],
    music: [0.4, 0.4, 0.4, 0.4, 0, 0, 0, 0]
  };

  function normalize(v) {
    const len = Math.sqrt(v.reduce((s, x) => s + x * x, 0)) || 1;
    return v.map(x => x / len);
  }

  function mockEmbed(text, dims = 8) {
    const words = String(text).toLowerCase().replace(/[^a-z0-9\s]/g, '').split(/\s+/).filter(Boolean);
    const vec = new Array(dims).fill(0);
    let hits = 0;
    for (const w of words) {
      if (CONCEPTS[w]) {
        for (let i = 0; i < dims; i++) vec[i] += CONCEPTS[w][i];
        hits++;
      }
    }
    if (hits === 0) {
      // fallback: deterministic hash-based vector
      for (let i = 0; i < dims; i++) {
        let n = 0;
        for (let j = 0; j < text.length; j++) n += text.charCodeAt(j) * (i + 1) * (j + 1);
        vec[i] = (n % 1000) / 1000;
      }
    }
    return new Float32Array(normalize(vec));
  }

  class VectorLayer {
    static IndexType = { FLAT: 0, IVF: 1, SLSH: 2 };
    static Distance = { L2: 0, COSINE: 1, DOT: 2 };

    static openSeparate(dbLocation, indexName, format = {}, runtime = {}) {
      const vl = new VectorLayer();
      vl._vectors = [];
      vl._dims = format.dims || 8;
      vl._distanceMetric = runtime.distance || VectorLayer.Distance.COSINE;
      return vl;
    }

    static open(indexName, db, format = {}, runtime = {}) {
      return VectorLayer.openSeparate(null, indexName, format, runtime);
    }

    _checkOpen() { if (this._closed) throw new Error('VectorLayer is closed'); }

    insertSync(id, vec, metadata) {
      this._checkOpen();
      if (!(vec instanceof Float32Array)) throw new TypeError('vec must be a Float32Array');
      this._vectors = this._vectors.filter(v => v.id !== id);
      this._vectors.push({ id, vec: Float32Array.from(vec), metadata: metadata || null });
    }

    _dist(a, b) {
      if (this._distanceMetric === VectorLayer.Distance.COSINE) {
        const dot = a.reduce((s, x, i) => s + x * b[i], 0);
        const na = Math.sqrt(a.reduce((s, x) => s + x * x, 0));
        const nb = Math.sqrt(b.reduce((s, x) => s + x * x, 0));
        return 1 - dot / (na * nb);
      }
      // L2 fallback
      return Math.sqrt(a.reduce((s, x, i) => s + (x - b[i]) ** 2, 0));
    }

    searchSync(query, k) {
      this._checkOpen();
      if (!(query instanceof Float32Array)) throw new TypeError('query must be a Float32Array');
      const ranked = this._vectors
        .map(v => ({ id: v.id, distance: this._dist(query, v.vec), metadata: v.metadata }))
        .sort((a, b) => a.distance - b.distance)
        .slice(0, k);
      return ranked;
    }

    deleteSync(id) {
      this._checkOpen();
      this._vectors = this._vectors.filter(v => v.id !== id);
    }

    count() { this._checkOpen(); return this._vectors.length; }
    train() {}
    rebuild() {}
    close() { this._closed = true; }
  }

  // ─────────────────────────────────────────────────────────────
  // Slide deck + code runner
  // ─────────────────────────────────────────────────────────────

  const slides = Array.from(document.querySelectorAll('.slide'));
  let current = 0;

  function showSlide(i) {
    if (i < 0) i = 0;
    if (i >= slides.length) i = slides.length - 1;
    current = i;
    slides.forEach((s, idx) => s.classList.toggle('active', idx === current));
    document.getElementById('progress-bar').style.width = `${((current + 1) / slides.length) * 100}%`;
    document.getElementById('counter').textContent = `${current + 1} / ${slides.length}`;
  }

  function runCode(textarea, terminal) {
    const code = textarea.value;
    terminal.innerHTML = '';

    const append = (type, parts) => {
      const line = parts.map(formatArg).join(' ');
      const el = document.createElement('div');
      el.className = type;
      el.textContent = line;
      terminal.appendChild(el);
      terminal.scrollTop = terminal.scrollHeight;
    };

    const sandboxConsole = {
      log: (...args) => append('log', args),
      info: (...args) => append('log', args),
      warn: (...args) => append('error', args),
      error: (...args) => append('error', args)
    };

    const run = async () => {
      const fn = new Function(
        'console', 'WaveDB', 'GraphQLLayer', 'GraphLayer', 'g', 'VectorLayer', 'mockEmbed',
        `"use strict";
        return (async () => {
          ${code}
        })();`
      );
      return await fn(sandboxConsole, WaveDB, GraphQLLayer, GraphLayer, g, VectorLayer, mockEmbed);
    };

    run()
      .then(value => {
        if (value !== undefined) append('result', ['=> ' + formatArg(value)]);
      })
      .catch(err => {
        append('error', [err.name ? `${err.name}: ${err.message}` : String(err)]);
      });
  }

  document.querySelectorAll('.demo-panel').forEach(panel => {
    const editor = panel.querySelector('.code-editor');
    const term = panel.querySelector('.terminal');
    const btn = panel.querySelector('.run-btn');
    if (btn) {
      btn.addEventListener('click', () => runCode(editor, term));
    }
  });

  document.getElementById('prev').addEventListener('click', () => showSlide(current - 1));
  document.getElementById('next').addEventListener('click', () => showSlide(current + 1));

  document.addEventListener('keydown', e => {
    if (['INPUT', 'TEXTAREA'].includes(document.activeElement.tagName)) return;
    switch (e.key) {
      case 'ArrowRight':
      case 'ArrowDown':
      case ' ':
      case 'PageDown':
        e.preventDefault();
        showSlide(current + 1);
        break;
      case 'ArrowLeft':
      case 'ArrowUp':
      case 'PageUp':
        e.preventDefault();
        showSlide(current - 1);
        break;
      case 'Home':
        e.preventDefault();
        showSlide(0);
        break;
      case 'End':
        e.preventDefault();
        showSlide(slides.length - 1);
        break;
    }
  });

  // Expose mocks for verification / debugging
  if (typeof window !== 'undefined') {
    window.WaveDB = WaveDB;
    window.GraphQLLayer = GraphQLLayer;
    window.GraphLayer = GraphLayer;
    window.g = g;
    window.VectorLayer = VectorLayer;
    window.mockEmbed = mockEmbed;
  }

  showSlide(0);
})();
