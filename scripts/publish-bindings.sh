#!/usr/bin/env bash
# publish-bindings.sh — top-level orchestrator for releasing all three bindings.
#
# Builds the release C library once, runs the C test suite, then publishes
# the Python and Node.js bindings (each with its own stale-check + canary +
# end-to-end verification). Dart is not published (uses a pre-built
# libwavedb.so) but its tests are run against the freshly built release lib.
#
# Usage:
#   scripts/publish-bindings.sh             # build release lib + publish Py + npm
#   scripts/publish-bindings.sh --no-upload # everything except the actual upload
#   scripts/publish-bindings.sh --check     # stale-check only, no build/upload
#
# This script is the single entry point for the release process. It exists
# so the "forgot to run copy_sources.py" failure mode (PyPI 0.1.7) and the
# "C++ wrapper bug slipped through" failure mode (npm 0.14.3) can't recur:
# each binding's publish.sh runs a stale-check, a canary test, and an
# end-to-end pull-from-registry verification before declaring success.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

CHECK_ONLY=0
NO_UPLOAD=0
for arg in "$@"; do
    case "$arg" in
        --check) CHECK_ONLY=1 ;;
        --no-upload) NO_UPLOAD=1 ;;
        *) echo "unknown arg: $arg" >&2; exit 2 ;;
    esac
done

if [ "$CHECK_ONLY" -eq 1 ]; then
    echo "=== Stale-check all c_src/ trees ==="
    scripts/check-c-src.sh
    exit 0
fi

EXTRA_ARGS=""
if [ "$NO_UPLOAD" -eq 1 ]; then
    EXTRA_ARGS="--no-upload"
fi

echo "=== [1/4] Build release C library ==="
if [ ! -d build-release ]; then
    cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -3
fi
cmake --build build-release -j 2>&1 | tail -3
RELEASE_LIB="build-release/libwavedb.so"
if [ ! -f "$RELEASE_LIB" ]; then
    echo "FAIL: $RELEASE_LIB not built." >&2
    exit 1
fi
echo "Release lib: $RELEASE_LIB ($(stat -c%s "$RELEASE_LIB") bytes)"

echo
echo "=== [2/4] Run C test suite ==="
# Build and run C tests in a debug build (tests are gated on BUILD_TESTS=ON).
if [ ! -d build-debug ]; then
    cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON 2>&1 | tail -3
fi
cmake --build build-debug -j 2>&1 | tail -3
( cd build-debug && ctest --output-on-failure 2>&1 | grep -E "tests passed|FAIL" | tail -3 )

echo
echo "=== [3/4] Publish Python binding ==="
bindings/python/scripts/publish.sh $EXTRA_ARGS

echo
echo "=== [4/4] Publish Node.js binding ==="
bindings/nodejs/scripts/publish.sh $EXTRA_ARGS

echo
echo "=== Bonus: Dart tests against the fresh release lib ==="
cp "$RELEASE_LIB" bindings/dart/libwavedb.so
( cd bindings/dart && dart test 2>&1 | tail -3 )

echo
echo "DONE: all bindings published and verified."
echo "  Release lib: $RELEASE_LIB"
echo "  C tests:     $(cd build-debug && ctest -N 2>/dev/null | grep -c 'Test #') tests"
echo "  Python:      https://pypi.org/project/wavedb/"
echo "  Node.js:     https://www.npmjs.com/package/@vijayee/wavedb"
echo "  Dart:        tests pass against fresh libwavedb.so (not published)"