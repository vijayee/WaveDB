#!/usr/bin/env bash
# publish.sh — mature Node.js binding publish flow.
#
# Wraps the full release sequence. npm's prepublishOnly hook already runs
# copy-sources + build + test, but this script adds:
#   - A stale-check that fails before upload if c_src/ is out of sync.
#   - A canary check that the built addon has the empty-value fix.
#   - An end-to-end verification that pulls from npm and re-tests.
#
# Usage:
#   bindings/nodejs/scripts/publish.sh            # publish current version
#   bindings/nodejs/scripts/publish.sh --no-upload # build + test, skip upload
#   bindings/nodejs/scripts/publish.sh --check     # stale-check only
#
# Env:
#   NPM_CONFIG_REGISTRY  optional; npm registry (default https://registry.npmjs.org)

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NODE_BIND_DIR="$(cd "$HERE/.." && pwd)"
REPO_ROOT="$(cd "$NODE_BIND_DIR/../.." && pwd)"

cd "$NODE_BIND_DIR"

UPLOAD=1
CHECK_ONLY=0
for arg in "$@"; do
    case "$arg" in
        --no-upload) UPLOAD=0 ;;
        --check) CHECK_ONLY=1 ;;
        *) echo "unknown arg: $arg" >&2; exit 2 ;;
    esac
done

echo "=== [1/6] Re-populate c_src/ from $REPO_ROOT/src/ ==="
npm run copy-sources 2>&1 | tail -3

echo
echo "=== [2/6] Stale-check c_src/ against src/ ==="
"$REPO_ROOT/scripts/check-c-src.sh"

if [ "$CHECK_ONLY" -eq 1 ]; then
    echo "--check requested; stopping after stale-check."
    exit 0
fi

echo
echo "=== [3/6] Build addon + run tests ==="
# prepublishOnly (copy-sources + build + test) runs automatically on `npm publish`,
# but run them explicitly here so failures surface before we get to upload.
npm run build 2>&1 | tail -3
npm test 2>&1 | tail -5

echo
echo "=== [4/6] Verify built addon has the empty-value fix (canary) ==="
# The C++ wrapper fix is in src/database.cc — check the built .node for the
# "return Napi::String::New(env, \"\")" path by testing it directly.
node -e "
const { WaveDB } = require('./lib/wavedb');
const fs = require('fs');
const os = require('os');
const path = require('path');
const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'wavedb-canary-'));
const db = new WaveDB(tmpDir);
db.putSync('a/b', '');
const val = db.getSync('a/b');
if (val !== '') { console.error('FAIL: empty value expected, got', JSON.stringify(val)); process.exit(1); }
db.close();
fs.rmSync(tmpDir, { recursive: true, force: true });
console.log('OK: empty-value roundtrip works in built addon.');
"

VERSION="$(node -p "require('./package.json').version")"

if [ "$UPLOAD" -eq 0 ]; then
    echo
    echo "--no-upload requested; stopping after local verification."
    echo "Version: $VERSION (not published)"
    exit 0
fi

echo
echo "=== [5/6] Publish to npm ==="
npm whoami >/dev/null 2>&1 || { echo "FAIL: npm whoami — not logged in. Run npm login." >&2; exit 1; }
npm publish --access public 2>&1 | tail -5

echo
echo "=== [6/6] Pull from npm and re-verify end-to-end ==="
sleep 5
VERIFY_DIR="$(mktemp -d)"
( cd "$VERIFY_DIR" \
  && echo '{"name":"wavedb-verify","version":"1.0.0"}' > package.json \
  && echo 'allow-scripts=true' > .npmrc \
  && npm install "@vijayee/wavedb@${VERSION}" 2>&1 | tail -3 \
  && node "node_modules/@vijayee/wavedb/scripts/build.js" 2>&1 | tail -3 \
  && node -e "
const { WaveDB } = require('@vijayee/wavedb');
const { GraphLayer } = require('@vijayee/wavedb/lib/graph');
const fs = require('fs');
const os = require('os');
const path = require('path');
const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'wavedb-npm-verify-'));
const db = new WaveDB(tmpDir);
db.putSync('a/b', '');
if (db.getSync('a/b') !== '') throw new Error('empty-value roundtrip');
const st = db.openSubtree('myns');
st.putSync('k', '');
if (st.getSync('k') !== '') throw new Error('subtree empty-value');
st.close();
for (let i = 0; i < 50; i++) db.putSync(\`users/u\${String(i).padStart(3,'0')}/name\`, \`n\${i}\`);
const stream = db.createReadStream({ start: 'users/' });
const keys = [];
stream.on('data', e => keys.push(e.key));
stream.on('end', () => {
  if (keys.length !== 50) throw new Error('expected 50 keys, got ' + keys.length);
  if (keys.some(k => k.includes('\0'))) throw new Error('NUL padding in keys');
  const st2 = db.openSubtree('graph');
  const g = new GraphLayer(null, { subtree: st2 });
  g.insertSync('alice', 'knows', 'bob');
  if (!g.exec('g.V(\"alice\").Out(\"knows\")').includes('bob')) throw new Error('graph query');
  g.close(); st2.close(); db.close();
  fs.rmSync(tmpDir, { recursive: true, force: true });
  console.log('npm @vijayee/wavedb@${VERSION} verified end-to-end.');
});
" 2>&1 | grep -v "INFO\|WARN\|ERROR" | tail -5 )
rm -rf "$VERIFY_DIR"

echo
echo "DONE: @vijayee/wavedb@${VERSION} published and verified on npm."
echo "  https://www.npmjs.com/package/@vijayee/wavedb/v/${VERSION}"