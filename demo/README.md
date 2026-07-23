# WaveDB Live Demo

A self-contained HTML5 slide deck and live-code playground that introduces WaveDB.

## How to run

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
