#!/usr/bin/env node
// Cross-platform build script for WaveDB Node.js bindings.
//
// On Windows: builds C static libraries with CMake+Ninja using the same MSVC
// toolchain as node-gyp, then runs node-gyp configure + patch + build.
//
// On Linux/macOS: runs node-gyp rebuild (which links pre-built .a files from
// the CMake build in the repo root, or triggers a CMake build if needed).
//
// The script also runs copy-sources.js when building from the monorepo to
// populate c_src/ for self-contained npm packages.

const { execSync } = require('child_process');
const fs = require('fs');
const path = require('path');

const isWin = process.platform === 'win32';
const scriptDir = __dirname;
const nodejsDir = path.join(scriptDir, '..');
const nodeGyp = path.join(nodejsDir, 'node_modules', '.bin', 'node-gyp');

function run(cmd) {
  execSync(cmd, { stdio: 'inherit' });
}

function findFileUpwards(startDir, filename) {
  let dir = startDir;
  while (true) {
    if (fs.existsSync(path.join(dir, filename))) return dir;
    const parent = path.resolve(dir, '..');
    if (parent === dir) return null;
    dir = parent;
  }
}

function isInMonorepo() {
  return findFileUpwards(nodejsDir, 'src/HBTrie/hbtrie.h') !== null;
}

function runCopySources() {
  const copyScript = path.join(scriptDir, 'copy-sources.js');
  if (fs.existsSync(copyScript)) {
    console.log('Running copy-sources.js...');
    run(`node "${copyScript}"`);
  }
}

// ── Windows: build C static libs with CMake, then node-gyp ──

function findVSPath() {
  // Try VS 2026 first, then VS 2022
  const candidates = [
    'C:\\Program Files (x86)\\Microsoft Visual Studio\\18\\BuildTools',
    'C:\\Program Files (x86)\\Microsoft Visual Studio\\18\\Community',
    'C:\\Program Files (x86)\\Microsoft Visual Studio\\18\\Enterprise',
    'C:\\Program Files\\Microsoft Visual Studio\\2022\\Community',
    'C:\\Program Files\\Microsoft Visual Studio\\2022\\BuildTools',
    'C:\\Program Files\\Microsoft Visual Studio\\2022\\Enterprise',
  ];
  for (const c of candidates) {
    if (fs.existsSync(path.join(c, 'VC', 'Auxiliary', 'Build', 'vcvarsall.bat'))) {
      return c;
    }
  }
  // Try vswhere
  try {
    const output = execSync(
      '"C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe" -latest -property installationPath',
      { encoding: 'utf8' }
    ).trim();
    if (output && fs.existsSync(path.join(output, 'VC', 'Auxiliary', 'Build', 'vcvarsall.bat'))) {
      return output;
    }
  } catch {}
  return null;
}

function findFiles(dir, ext) {
  const results = [];
  function walk(d) {
    if (!fs.existsSync(d)) return;
    for (const entry of fs.readdirSync(d, { withFileTypes: true })) {
      const full = path.join(d, entry.name);
      if (entry.isDirectory() && entry.name !== 'node_modules') walk(full);
      else if (entry.isFile() && entry.name.endsWith(ext)) results.push(full);
    }
  }
  walk(dir);
  return results;
}

function patchVcxprojForMSVC() {
  const buildDir = path.join(nodejsDir, 'build');
  const vcxprojFiles = findFiles(buildDir, '.vcxproj');

  for (const file of vcxprojFiles) {
    let content = fs.readFileSync(file, 'utf8');
    let modified = false;

    // Remove LLD-specific LTO options that MSVC link.exe doesn't understand
    if (content.includes('/opt:lldltojobs=')) {
      content = content.replace(/\/opt:lldltojobs=\d+\s*/g, '');
      modified = true;
    }
    if (content.includes('-flto=thin')) {
      content = content.replace(/-flto=thin\s*/g, '');
      modified = true;
    }

    if (modified) {
      fs.writeFileSync(file, content);
    }
  }
}

function buildStaticLibsForMSVC() {
  const repoRoot = findFileUpwards(nodejsDir, 'src/HBTrie/hbtrie.h');
  if (!repoRoot) {
    console.error('Cannot find WaveDB source tree. Build requires the monorepo.');
    process.exit(1);
  }

  const buildDir = path.join(repoRoot, 'build-nodejs');

  const vsPath = findVSPath();
  if (!vsPath) {
    console.error('Could not find Visual Studio installation for MSVC build');
    process.exit(1);
  }

  const vcvarsall = path.join(vsPath, 'VC', 'Auxiliary', 'Build', 'vcvarsall.bat');
  if (!fs.existsSync(vcvarsall)) {
    console.error('Could not find vcvarsall.bat at', vcvarsall);
    process.exit(1);
  }

  // Build using CMake + Ninja — use /MT (static CRT) to match node-gyp
  const cmd = [
    `"${vcvarsall}" x64`,
    '&&',
    `cmake -S "${repoRoot}" -B "${buildDir}" -G Ninja`,
    `-DCMAKE_BUILD_TYPE=Release`,
    `-DCMAKE_C_FLAGS_RELEASE="/MT /O2 /Ob2 /DNDEBUG"`,
    `-DCMAKE_CXX_FLAGS_RELEASE="/MT /O2 /Ob2 /DNDEBUG"`,
    `-DBUILD_TESTING=OFF -DBUILD_BENCHMARKS=OFF`,
    '&&',
    `cmake --build "${buildDir}" --target wavedb cbor xxhash hashmap`,
  ].join(' ');

  try {
    execSync(cmd, { stdio: 'inherit' });
  } catch (e) {
    console.error('Failed to build WaveDB static libraries');
    process.exit(1);
  }

  // Copy static libraries to node-gyp build output directory
  const nodeGypBuildDir = path.join(nodejsDir, 'build', 'Release');
  if (!fs.existsSync(nodeGypBuildDir)) {
    fs.mkdirSync(nodeGypBuildDir, { recursive: true });
  }

  const libs = [
    { src: 'wavedb.lib', dst: 'wavedb_core.lib' },
    { src: path.join('deps', 'libcbor', 'src', 'cbor.lib'), dst: 'wavedb_cbor.lib' },
    { src: 'xxhash.lib', dst: 'wavedb_xxhash.lib' },
    { src: 'hashmap.lib', dst: 'wavedb_hashmap.lib' },
  ];

  // Also check top-level cbor.lib location
  const altCborSrc = path.join(buildDir, 'cbor.lib');

  for (const lib of libs) {
    const srcPath = path.join(buildDir, lib.src);
    if (fs.existsSync(srcPath)) {
      fs.copyFileSync(srcPath, path.join(nodeGypBuildDir, lib.dst));
    } else if (lib.dst === 'wavedb_cbor.lib' && fs.existsSync(altCborSrc)) {
      fs.copyFileSync(altCborSrc, path.join(nodeGypBuildDir, lib.dst));
    }
  }
}

// ── Linux/macOS: ensure pre-built static libs exist ──

function buildStaticLibsForUnix() {
  const repoRoot = findFileUpwards(nodejsDir, 'src/HBTrie/hbtrie.h');
  if (!repoRoot) {
    console.error('Cannot find WaveDB source tree. Build requires the monorepo.');
    process.exit(1);
  }

  const buildDir = path.join(repoRoot, 'build');
  const libwavedb = path.join(buildDir, 'libwavedb.a');

  if (fs.existsSync(libwavedb)) {
    console.log('Using pre-built static libraries from', buildDir);
    return;
  }

  // No pre-built libs — build with CMake
  console.log('Pre-built libraries not found. Building with CMake...');
  const cmd = [
    `cmake -S "${repoRoot}" -B "${buildDir}"`,
    '-DCMAKE_BUILD_TYPE=Release',
    '-DBUILD_TESTING=OFF -DBUILD_BENCHMARKS=OFF',
    '&&',
    `cmake --build "${buildDir}" --target wavedb cbor xxhash hashmap`,
  ].join(' ');

  try {
    execSync(cmd, { stdio: 'inherit' });
  } catch (e) {
    console.error('Failed to build WaveDB static libraries');
    process.exit(1);
  }
}

// ── Main build flow ──

if (isWin) {
  buildStaticLibsForMSVC();
  run(`"${nodeGyp}" configure`);
  patchVcxprojForMSVC();
  run(`"${nodeGyp}" build`);
} else {
  buildStaticLibsForUnix();
  run(`"${nodeGyp}" rebuild`);
}