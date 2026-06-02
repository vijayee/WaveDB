#!/bin/bash
# Copy C sources from the monorepo into bindings/nodejs/c_src/ for npm packaging.
# Must be run from the repo root.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NODEJS_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$NODEJS_DIR/../.." && pwd)"
C_SRC="$NODEJS_DIR/c_src"

# Validate we're in the right place
if [ ! -f "$REPO_ROOT/CMakeLists.txt" ]; then
    echo "Error: Cannot find CMakeLists.txt at $REPO_ROOT"
    echo "Run this script from the WaveDB repo root or bindings/nodejs directory"
    exit 1
fi

echo "Copying C sources from $REPO_ROOT to $C_SRC"

# Clean previous copy
rm -rf "$C_SRC"
mkdir -p "$C_SRC"

# Copy WaveDB core sources (preserving directory structure)
cp -r "$REPO_ROOT/src" "$C_SRC/src"

# Copy xxhash
mkdir -p "$C_SRC/deps/xxhash"
cp "$REPO_ROOT/deps/xxhash/xxhash.c" "$C_SRC/deps/xxhash/"
cp "$REPO_ROOT/deps/xxhash/xxhash.h" "$C_SRC/deps/xxhash/"
# xxh3.h is included by xxhash.h
if [ -f "$REPO_ROOT/deps/xxhash/xxh3.h" ]; then
    cp "$REPO_ROOT/deps/xxhash/xxh3.h" "$C_SRC/deps/xxhash/"
fi
if [ -f "$REPO_ROOT/deps/xxhash/xxh_x86dispatch.c" ]; then
    cp "$REPO_ROOT/deps/xxhash/xxh_x86dispatch.c" "$C_SRC/deps/xxhash/"
fi
if [ -f "$REPO_ROOT/deps/xxhash/xxh_x86dispatch.h" ]; then
    cp "$REPO_ROOT/deps/xxhash/xxh_x86dispatch.h" "$C_SRC/deps/xxhash/"
fi

# Copy hashmap
mkdir -p "$C_SRC/deps/hashmap/src"
mkdir -p "$C_SRC/deps/hashmap/include"
cp "$REPO_ROOT/deps/hashmap/src/hashmap.c" "$C_SRC/deps/hashmap/src/"
cp "$REPO_ROOT/deps/hashmap/include/hashmap.h" "$C_SRC/deps/hashmap/include/"
cp "$REPO_ROOT/deps/hashmap/include/hashmap_base.h" "$C_SRC/deps/hashmap/include/"

# Copy libcbor (preserving directory structure)
mkdir -p "$C_SRC/deps/libcbor"
cp -r "$REPO_ROOT/deps/libcbor/src" "$C_SRC/deps/libcbor/src"

# Remove test/example files from libcbor
rm -rf "$C_SRC/deps/libcbor/src/test" "$C_SRC/deps/libcbor/src/example" 2>/dev/null || true

# Create static configuration.h (replaces CMake-generated file)
mkdir -p "$C_SRC/deps/libcbor/src/cbor"
cat > "$C_SRC/deps/libcbor/src/cbor/configuration.h" << 'EOF'
#ifndef LIBCBOR_CONFIGURATION_H
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
EOF

echo "Done. C sources copied to $C_SRC"