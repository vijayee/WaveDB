'use strict';

const path = require('path');
const { execSync } = require('child_process');

const bindingsDir = path.resolve(__dirname, '..', '..', '..', 'bindings', 'nodejs');

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
