#!/usr/bin/env node
// Cross-platform script to copy C sources from the monorepo into c_src/
// for npm packaging. Works on Windows, macOS, and Linux.
//
// Run from the WaveDB repo root or from bindings/nodejs/:
//   node scripts/copy-sources.js
//   npm run copy-sources

const fs = require('fs');
const path = require('path');

// Determine repo root: look for src/HBTrie/hbtrie.h (unique to WaveDB root)
let repoRoot = process.cwd();
while (!fs.existsSync(path.join(repoRoot, 'src', 'HBTrie', 'hbtrie.h'))) {
  const parent = path.resolve(repoRoot, '..');
  if (parent === repoRoot) {
    console.error('Error: Cannot find WaveDB source tree. Run from the WaveDB repo root or bindings/nodejs/');
    process.exit(1);
  }
  repoRoot = parent;
}

const cSrcDir = path.join(repoRoot, 'bindings', 'nodejs', 'c_src');

console.log('Copying C sources from', repoRoot, 'to', cSrcDir);

// Clean previous copy
fs.rmSync(cSrcDir, { recursive: true, force: true });

// Helper: copy file ensuring directory exists
function copyFile(src, dest) {
  fs.mkdirSync(path.dirname(dest), { recursive: true });
  fs.copyFileSync(src, dest);
}

// Helper: copy directory recursively
function copyDir(src, dest) {
  fs.mkdirSync(dest, { recursive: true });
  for (const entry of fs.readdirSync(src, { withFileTypes: true })) {
    const srcPath = path.join(src, entry.name);
    const destPath = path.join(dest, entry.name);
    if (entry.isDirectory()) {
      copyDir(srcPath, destPath);
    } else if (entry.isFile()) {
      fs.copyFileSync(srcPath, destPath);
    }
  }
}

// 1. Copy WaveDB core sources (preserving directory structure)
copyDir(path.join(repoRoot, 'src'), path.join(cSrcDir, 'src'));

// 2. Copy xxhash
const xxhashDir = path.join(cSrcDir, 'deps', 'xxhash');
fs.mkdirSync(xxhashDir, { recursive: true });
for (const f of ['xxhash.c', 'xxhash.h', 'xxh3.h']) {
  const src = path.join(repoRoot, 'deps', 'xxhash', f);
  if (fs.existsSync(src)) copyFile(src, path.join(xxhashDir, f));
}
// xxh_x86dispatch is optional but included if present
for (const f of ['xxh_x86dispatch.c', 'xxh_x86dispatch.h']) {
  const src = path.join(repoRoot, 'deps', 'xxhash', f);
  if (fs.existsSync(src)) copyFile(src, path.join(xxhashDir, f));
}

// 3. Copy hashmap
fs.mkdirSync(path.join(cSrcDir, 'deps', 'hashmap', 'src'), { recursive: true });
fs.mkdirSync(path.join(cSrcDir, 'deps', 'hashmap', 'include'), { recursive: true });
copyFile(
  path.join(repoRoot, 'deps', 'hashmap', 'src', 'hashmap.c'),
  path.join(cSrcDir, 'deps', 'hashmap', 'src', 'hashmap.c')
);
copyFile(
  path.join(repoRoot, 'deps', 'hashmap', 'include', 'hashmap.h'),
  path.join(cSrcDir, 'deps', 'hashmap', 'include', 'hashmap.h')
);
copyFile(
  path.join(repoRoot, 'deps', 'hashmap', 'include', 'hashmap_base.h'),
  path.join(cSrcDir, 'deps', 'hashmap', 'include', 'hashmap_base.h')
);

// 4. Copy libcbor (preserving directory structure)
copyDir(
  path.join(repoRoot, 'deps', 'libcbor', 'src'),
  path.join(cSrcDir, 'deps', 'libcbor', 'src')
);

// Remove test/example dirs from libcbor copy
for (const dir of ['test', 'example', 'tests', 'examples']) {
  const d = path.join(cSrcDir, 'deps', 'libcbor', 'src', dir);
  if (fs.existsSync(d)) fs.rmSync(d, { recursive: true, force: true });
}

// 5. Create static configuration.h (replaces CMake-generated file)
const configDir = path.join(cSrcDir, 'deps', 'libcbor', 'src', 'cbor');
fs.mkdirSync(configDir, { recursive: true });
fs.writeFileSync(path.join(configDir, 'configuration.h'), `#ifndef LIBCBOR_CONFIGURATION_H
#define LIBCBOR_CONFIGURATION_H

#define CBOR_MAJOR_VERSION 0
#define CBOR_MINOR_VERSION 13
#define CBOR_PATCH_VERSION 0

#define CBOR_BUFFER_GROWTH 2
#define CBOR_MAX_STACK_SIZE 2048
#define CBOR_PRETTY_PRINTER 1

#ifndef CBOR_RESTRICT_SPECIFIER
#ifdef _MSC_VER
#define CBOR_RESTRICT_SPECIFIER
#else
#define CBOR_RESTRICT_SPECIFIER restrict
#endif
#endif

#endif // LIBCBOR_CONFIGURATION_H
`);

console.log('Done. C sources copied to', cSrcDir);