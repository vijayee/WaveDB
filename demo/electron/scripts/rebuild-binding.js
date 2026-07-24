'use strict';

const path = require('path');
const { execSync } = require('child_process');

const bindingsDir = path.resolve(__dirname, '..', '..', '..', 'bindings', 'nodejs');

// The binding.gyp references sources under bindings/nodejs/c_src/, which is
// a trimmed copy of the monorepo sources. Make sure it is up to date before
// rebuilding, otherwise recently added source directories (e.g. vector) will
// be missing.
console.log('Ensuring c_src sources are copied...');
try {
  execSync('node scripts/copy-sources.js', {
    cwd: bindingsDir,
    stdio: 'inherit',
    shell: true,
    windowsHide: true
  });
} catch (err) {
  console.error('copy-sources.js failed:', err.message);
  process.exit(err.status || 1);
}

// Resolve the Electron version from the locally installed package.
const electronModuleDir = path.resolve(__dirname, '..', 'node_modules', 'electron');
let electronVersion;
try {
  const pkg = require(path.join(electronModuleDir, 'package.json'));
  electronVersion = pkg.version;
} catch (err) {
  console.error('Could not determine Electron version. Make sure electron is installed in demo/electron.');
  process.exit(1);
}

console.log(`Rebuilding WaveDB binding for Electron ${electronVersion}...`);
console.log(`Bindings directory: ${bindingsDir}`);

// The published tarball does not bundle OpenSSL headers, so the default
// Windows build of @vijayee/wavedb skips OpenSSL. Users who want encryption
// can install OpenSSL, set WAVEDB_OPENSSL=1 and OPENSSL_ROOT, and rerun
// `npm install` / `npm run rebuild`.
const cmd = [
  'node-gyp',
  'rebuild',
  `--target=${electronVersion}`,
  '--arch=x64',
  '--dist-url=https://electronjs.org/headers',
  '--release'
].join(' ');

try {
  execSync(cmd, {
    cwd: bindingsDir,
    stdio: 'inherit',
    shell: true,
    windowsHide: true
  });
  console.log('Rebuild succeeded.');
} catch (err) {
  console.error('Rebuild failed:', err.message);
  process.exit(err.status || 1);
}
