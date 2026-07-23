# WaveDB Live Demo

A self-contained HTML5 slide deck and live-code playground that introduces WaveDB.

## How to run

### Browser mode (mock engine)

No build step or native dependency is required. Just open `index.html` in a modern browser:

```bash
cd demo
# On macOS / Linux
open index.html

# On Windows
start index.html
# or open it directly in your browser from File Explorer
```

Or serve it through any static file server:

```bash
cd demo
python -m http.server 8080
# then visit http://localhost:8080
```

### Electron mode (real WaveDB engine)

The `demo/electron/` harness loads the same slide deck but runs the native WaveDB engine in the main process. The renderer communicates with it over synchronous IPC.

Prerequisites:
- Node.js and the WaveDB Node.js bindings are built (`bindings/nodejs/build/Release/*.node`).
- If the native binaries were built for a different Node version than Electron ships, run `npm run rebuild` below.

```bash
cd demo/electron
npm install          # installs Electron + rebuild tools
npm run rebuild      # rebuilds the native binding for Electron's Node ABI
npm start            # launches the presentation
```

When running in Electron, a `⚡ Real WaveDB engine` badge appears in the top-right corner and every live code snippet executes against the real C++ database.

If `npm install` fails to download the Electron binary or the native module does not load, see [`electron/TROUBLESHOOTING.md`](electron/TROUBLESHOOTING.md).

## What it demonstrates

1. **Base WaveDB layer** — store hierarchical keys, JSON objects, raw values, and atomic batches.
2. **GraphQL schema layer** — parse an SDL, mutate data, and query it back.
3. **Graph schema layer** — insert/delete triples and traverse them with a Gremlin-style DSL.
4. **Vector schema layer** — insert mood embeddings and run nearest-neighbour "vibe search" over tracks.
5. **Conclusion** — links to the GitHub repository and contact email.

- Repository: https://github.com/vijayee/WaveDB
- Contact: victor.j.morrow@gmail.com

## Browser APIs used

- Keyboard navigation (arrow keys, space, page up/down, home, end).
- A mocked `WaveDB`, `GraphQLLayer`, `GraphLayer`, `VectorLayer`, and `g` traversal source so the code examples execute in-browser without the native Node.js binding.

## Styling

Colors are derived from the WaveDB logo (`#8290c7`, `#7471bd`, `#eb8897`, `#f9f8ee`).
