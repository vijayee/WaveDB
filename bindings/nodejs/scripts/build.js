const { execSync } = require('child_process');
const fs = require('fs');
const path = require('path');

const isWin = process.platform === 'win32';

const nodeGyp = path.join(__dirname, '..', 'node_modules', '.bin', 'node-gyp');

function run(cmd) {
  execSync(cmd, { stdio: 'inherit' });
}

function patchVcxprojForMSVC() {
  const buildDir = path.join(__dirname, '..', 'build');
  const vcxprojFiles = findFiles(buildDir, '.vcxproj');

  for (const file of vcxprojFiles) {
    let content = fs.readFileSync(file, 'utf8');
    let modified = false;

    // Remove LLD-specific LTO options
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

function buildStaticLibraryForMSVC() {
  // Build the WaveDB C static library using CMake + Ninja with the same
  // MSVC toolchain that node-gyp uses. This ensures CRT compatibility.
  const buildDir = path.join(__dirname, '..', '..', '..', 'build-nodejs');
  const srcDir = path.join(__dirname, '..', '..', '..');

  if (!fs.existsSync(buildDir)) {
    fs.mkdirSync(buildDir, { recursive: true });
  }

  // Use the VS 2026 developer command environment to run CMake + Ninja
  // First, find the vcvarsall.bat to set up the MSVC environment
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

  // Build using CMake + Ninja through the developer command prompt
  // Use /MT (static CRT) to match node-gyp's default CRT linkage
  const cmd = [
    `"${vcvarsall}" x64`,
    '&&',
    `cmake -S "${srcDir}" -B "${buildDir}" -G Ninja`,
    `-DCMAKE_BUILD_TYPE=Release`,
    `-DCMAKE_C_FLAGS_RELEASE="/MT /O2 /Ob2 /DNDEBUG"`,
    `-DCMAKE_CXX_FLAGS_RELEASE="/MT /O2 /Ob2 /DNDEBUG"`,
    `-DBUILD_TESTING=OFF -DBUILD_BENCHMARKS=OFF`,
    '&&',
    `cmake --build "${buildDir}" --target wavedb cbor xxhash hashmap`
  ].join(' ');

  try {
    execSync(cmd, { stdio: 'inherit' });
  } catch (e) {
    console.error('Failed to build WaveDB static library');
    process.exit(1);
  }

  // Copy the static libraries to the node-gyp build output directory
  const nodeGypBuildDir = path.join(__dirname, '..', 'build', 'Release');
  if (!fs.existsSync(nodeGypBuildDir)) {
    fs.mkdirSync(nodeGypBuildDir, { recursive: true });
  }

  const libs = [
    { src: 'wavedb.lib', dst: 'wavedb_core.lib' },
    { src: 'cbor.lib', dst: 'wavedb_cbor.lib' },
    { src: path.join('deps', 'libcbor', 'src', 'cbor.lib'), dst: 'wavedb_cbor.lib' },
    { src: 'xxhash.lib', dst: 'wavedb_xxhash.lib' },
    { src: 'hashmap.lib', dst: 'wavedb_hashmap.lib' },
  ];

  for (const lib of libs) {
    const srcPath = path.join(buildDir, lib.src);
    if (fs.existsSync(srcPath)) {
      fs.copyFileSync(srcPath, path.join(nodeGypBuildDir, lib.dst));
    }
  }
}

function findVSPath() {
  // Try to find VS 2026 or VS 2022 installation
  const candidates = [
    'C:\\Program Files (x86)\\Microsoft Visual Studio\\18\\BuildTools',
    'C:\\Program Files\\Microsoft Visual Studio\\2022\\Community',
    'C:\\Program Files\\Microsoft Visual Studio\\2022\\BuildTools',
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

if (isWin) {
  // On Windows:
  // 1. Build the C static library with the same MSVC toolchain node-gyp uses
  // 2. Run node-gyp configure + patch LTO options + build
  buildStaticLibraryForMSVC();
  run(`"${nodeGyp}" configure`);
  patchVcxprojForMSVC();
  run(`"${nodeGyp}" build`);
} else {
  run(`"${nodeGyp}" rebuild`);
}