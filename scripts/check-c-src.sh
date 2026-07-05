#!/usr/bin/env bash
# check-c-src.sh — verify bindings/*/c_src/ is in sync with the repo's src/.
#
# Exits 0 if both Python and Node.js c_src/ trees match src/, 1 otherwise.
# Run before publishing bindings, after pulling, or in CI to catch the
# "forgot to run copy_sources.py" failure mode that shipped stale C sources
# to PyPI (wavedb 0.1.7) and would ship stale sources to npm without the
# prepublishOnly hook.
#
# Usage: scripts/check-c-src.sh [--fix]
#   --fix  re-run copy_sources.py / copy-sources.js to repopulate c_src/
#          instead of just reporting staleness

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

PY_CSRC="bindings/python/c_src/src"
NODE_CSRC="bindings/nodejs/c_src/src"
SRC="src"

stale=0

check_dir() {
    local label="$1" local_dir="$2"
    if [ ! -d "$local_dir" ]; then
        echo "STALE [$label]: $local_dir does not exist — run the binding's copy_sources script."
        stale=1
        return
    fi
    # diff against the canonical src/ tree. --brief skips content, -q exits 1 on any diff.
    # Exclude nothing — copy_sources.py copies the full src/ tree, so any diff is staleness.
    if ! diff -rq "$SRC" "$local_dir" >/dev/null 2>&1; then
        echo "STALE [$label]: $local_dir differs from $SRC/"
        diff -rq "$SRC" "$local_dir" 2>&1 | head -20 | sed 's/^/  /'
        stale=1
    else
        echo "OK    [$label]: $local_dir in sync with $SRC/"
    fi
}

check_dir "python" "$PY_CSRC"
check_dir "nodejs" "$NODE_CSRC"

if [ "$stale" -ne 0 ]; then
    if [ "${1:-}" = "--fix" ]; then
        echo
        echo "Re-populating c_src/ from $SRC/..."
        ( cd bindings/python && python3 scripts/copy_sources.py )
        ( cd bindings/nodejs && npm run copy-sources >/dev/null 2>&1 || node scripts/copy-sources.js )
        # Re-check after fix
        stale=0
        check_dir "python" "$PY_CSRC"
        check_dir "nodejs" "$NODE_CSRC"
        if [ "$stale" -ne 0 ]; then
            echo "FAIL: c_src/ still stale after --fix; copy_sources scripts may be broken."
            exit 1
        fi
        echo "FIXED: c_src/ re-populated and verified in sync."
        exit 0
    fi
    echo
    echo "FAIL: c_src/ is stale. Run one of:"
    echo "  scripts/check-c-src.sh --fix"
    echo "  ( cd bindings/python && python3 scripts/copy_sources.py )"
    echo "  ( cd bindings/nodejs && npm run copy-sources )"
    exit 1
fi

echo
echo "OK: all c_src/ trees in sync with $SRC/."