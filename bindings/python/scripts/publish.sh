#!/usr/bin/env bash
# publish.sh — mature Python binding publish flow.
#
# Wraps the full release sequence so copy_sources.py can't be forgotten
# (which shipped stale C sources to PyPI as wavedb 0.1.7):
#   1. Re-run copy_sources.py from the repo root.
#   2. Stale-check: diff c_src/ against src/, fail if out of sync.
#   3. Locate the release libwavedb.so (WAVEDB_LIB_PATH or auto-discover
#      under ../../build*/).
#   4. Build sdist + wheel with the release lib.
#   5. Verify the sdist's c_src/ has the empty-value fix (canary for
#      "did copy_sources actually run").
#   6. Install the wheel into --user site-packages and run the test suite
#      against the installed package (not the source tree).
#   7. Upload the sdist to PyPI with twine.
#   8. Pull from PyPI and re-run the test suite against the registry package.
#
# Usage:
#   bindings/python/scripts/publish.sh            # publish current version
#   bindings/python/scripts/publish.sh --no-upload # build + test, skip upload
#   bindings/python/scripts/publish.sh --check     # stale-check only
#
# Env:
#   WAVEDB_LIB_PATH  optional; pin a specific libwavedb.so. Otherwise
#                     auto-discovers the newest build*/libwavedb.so.
#   TWINE_*           twine credentials (or ~/.pypirc).

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PY_BIND_DIR="$(cd "$HERE/.." && pwd)"
REPO_ROOT="$(cd "$PY_BIND_DIR/../.." && pwd)"

cd "$PY_BIND_DIR"

UPLOAD=1
CHECK_ONLY=0
for arg in "$@"; do
    case "$arg" in
        --no-upload) UPLOAD=0 ;;
        --check) CHECK_ONLY=1 ;;
        *) echo "unknown arg: $arg" >&2; exit 2 ;;
    esac
done

echo "=== [1/8] Re-populate c_src/ from $REPO_ROOT/src/ ==="
python3 scripts/copy_sources.py | tail -2

echo
echo "=== [2/8] Stale-check c_src/ against src/ ==="
"$REPO_ROOT/scripts/check-c-src.sh"

if [ "$CHECK_ONLY" -eq 1 ]; then
    echo "--check requested; stopping after stale-check."
    exit 0
fi

echo
echo "=== [3/8] Locate release libwavedb.so ==="
LIB_PATH="${WAVEDB_LIB_PATH:-}"
if [ -z "$LIB_PATH" ]; then
    # Auto-discover: newest build*/libwavedb.so under REPO_ROOT, prefer
    # canonical name. Mirrors setup.py's auto-discovery logic.
    LIB_PATH="$(find "$REPO_ROOT" -maxdepth 2 -name 'libwavedb.so' \
                 -path 'build*/libwavedb.so' 2>/dev/null \
                 | xargs ls -t 2>/dev/null | head -1 || true)"
    if [ -z "$LIB_PATH" ]; then
        echo "FAIL: no libwavedb.so found under $REPO_ROOT/build*/. Run:" >&2
        echo "  cmake -S $REPO_ROOT -B $REPO_ROOT/build-release -DCMAKE_BUILD_TYPE=Release" >&2
        echo "  cmake --build $REPO_ROOT/build-release -j" >&2
        echo "or set WAVEDB_LIB_PATH=/path/to/libwavedb.so" >&2
        exit 1
    fi
fi
if [ ! -f "$LIB_PATH" ]; then
    echo "FAIL: WAVEDB_LIB_PATH=$LIB_PATH does not exist." >&2
    exit 1
fi
echo "Using: $LIB_PATH"

echo
echo "=== [4/8] Build sdist + wheel ==="
rm -rf dist build src/wavedb/_lib/*.so src/wavedb/_lib/*.dylib src/wavedb/_lib/*.dll 2>/dev/null || true
# Restore _lib/__init__.py if rm -rf _lib wiped it (the build needs the
# package marker; git restore brings back the tracked file).
if [ ! -f src/wavedb/_lib/__init__.py ]; then
    ( cd "$REPO_ROOT" && git restore bindings/python/src/wavedb/_lib/__init__.py )
fi
WAVEDB_LIB_PATH="$LIB_PATH" python3 -m build 2>&1 | tail -5

echo
echo "=== [5/8] Verify sdist has the empty-value fix (copy_sources canary) ==="
SDIST="$(ls dist/wavedb-*.tar.gz | head -1)"
VERSION="$(basename "$SDIST" | sed 's/^wavedb-//; s/\.tar\.gz$//')"
TMP_EXTRACT="$(mktemp -d)"
tar xzf "$SDIST" -C "$TMP_EXTRACT"
if ! grep -q "identifier_get_data_copy returns NULL for a valid empty value" \
        "$TMP_EXTRACT/wavedb-$VERSION/c_src/src/Database/database.c" 2>/dev/null; then
    echo "FAIL: sdist c_src/src/Database/database.c missing the empty-value fix." >&2
    echo "       copy_sources.py didn't copy the latest C sources." >&2
    rm -rf "$TMP_EXTRACT"
    exit 1
fi
# Also verify _lib/__init__.py is in the wheel (without it, importlib.resources
# can't locate the bundled libwavedb.so — broke 0.1.5).
WHEEL="$(ls dist/wavedb-*.whl | head -1)"
if ! unzip -l "$WHEEL" | grep -q "wavedb/_lib/__init__.py"; then
    echo "FAIL: wheel missing wavedb/_lib/__init__.py — package loader will break." >&2
    exit 1
fi
rm -rf "$TMP_EXTRACT"
echo "OK: sdist has empty-value fix; wheel has _lib/__init__.py."

echo
echo "=== [6/8] Install wheel and run tests against installed package ==="
pip install --user --force-reinstall "$WHEEL" 2>&1 | tail -3
# Run tests against the installed package, NOT the source tree (PYTHONPATH
# would shadow the installed package and hide wheel-only bugs).
( cd "$PY_BIND_DIR" && python3 -m pytest tests/ -q 2>&1 | tail -3 )

if [ "$UPLOAD" -eq 0 ]; then
    echo
    echo "--no-upload requested; stopping after local verification."
    echo "Artifacts in $PY_BIND_DIR/dist/"
    exit 0
fi

echo
echo "=== [7/8] Upload sdist to PyPI ==="
twine upload "$SDIST" 2>&1 | tail -3

echo
echo "=== [8/8] Pull from PyPI and re-verify ==="
sleep 5  # give PyPI a moment to propagate
pip install --user --upgrade "wavedb==${VERSION}" 2>&1 | tail -3
python3 -c "
import wavedb
from wavedb import WaveDB, GraphLayer
assert wavedb.__version__ == '${VERSION}', f'version mismatch: {wavedb.__version__}'
import tempfile
with tempfile.TemporaryDirectory() as d:
    db = WaveDB(d)
    db.put_sync('a/b', '')
    assert db.get_sync('a/b') == b'', 'empty-value roundtrip failed'
    for i in range(50):
        db.put_sync(f'users/u{i:03d}/name', f'n{i}')
    keys = [k for k, _ in db.create_read_stream(start='users/')]
    assert len(keys) == 50 and not any(chr(0) in k for k in keys), 'scan padding'
    g = GraphLayer('graph', db)
    g.insert_sync('alice', 'knows', 'bob')
    assert 'bob' in g.query().vertex('alice').out('knows').execute_sync().vertices
    g.close(); db.close()
print(f'PyPI wavedb==${VERSION} verified end-to-end.')
"

echo
echo "DONE: wavedb==${VERSION} published and verified on PyPI."
echo "  https://pypi.org/project/wavedb/${VERSION}/"