#!/bin/bash
# benchmark/compare.sh - Compare Dart and Node.js performance

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

echo "WaveDB Performance Comparison"
echo "=============================="
echo ""

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Check for dependencies
check_dependencies() {
    if ! command -v node &> /dev/null; then
        echo -e "${RED}Error: Node.js not found${NC}"
        exit 1
    fi
    if ! command -v dart &> /dev/null; then
        echo -e "${RED}Error: Dart not found${NC}"
        exit 1
    fi
}

# Build native library if needed
build_native_library() {
    echo -e "${YELLOW}Building native library...${NC}"
    cd "$PROJECT_ROOT"
    if [ ! -f "build/libwavedb.so" ] && [ ! -f "build/libwavedb.dylib" ]; then
        mkdir -p build
        cd build
        cmake ..
        make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
    fi
    cd "$SCRIPT_DIR"
}

# Run Node.js benchmark
run_nodejs_benchmark() {
    echo -e "${YELLOW}Running Node.js benchmark...${NC}"
    cd "$PROJECT_ROOT/bindings/nodejs"

    # Set library path
    export LD_LIBRARY_PATH="$PROJECT_ROOT/build:$LD_LIBRARY_PATH"
    export DYLD_LIBRARY_PATH="$PROJECT_ROOT/build:$DYLD_LIBRARY_PATH"

    node benchmark.js 2>&1 | tee "$SCRIPT_DIR/nodejs_results.txt"
    echo ""
}

# Run Dart benchmark
run_dart_benchmark() {
    echo -e "${YELLOW}Running Dart benchmark...${NC}"
    cd "$PROJECT_ROOT/bindings/dart"

    # Set library path
    export LD_LIBRARY_PATH="$PROJECT_ROOT/build:$LD_LIBRARY_PATH"
    export DYLD_LIBRARY_PATH="$PROJECT_ROOT/build:$DYLD_LIBRARY_PATH"

    # Run benchmark
    dart run benchmark/benchmark.dart 2>&1 | tee "$SCRIPT_DIR/dart_results.txt"
    echo ""
}

# Parse results and compare
compare_results() {
    echo ""
    echo "Performance Comparison"
    echo "======================="
    echo ""

    # Extract ops/sec from both results
    extract_ops() {
        local file="$1"
        local operation="$2"
        grep -E "^\s*$operation:" "$file" 2>/dev/null | sed 's/[^0-9]*\([0-9]*\).*/\1/' | head -1
    }

    # Compare each operation
    operations=("put" "get" "putSync" "getSync" "batch" "stream")

    printf "%-12s %12s %12s %12s\n" "Operation" "Node.js" "Dart" "Ratio"
    printf "%-12s %12s %12s %12s\n" "---------" "-------" "----" "-----"

    for op in "${operations[@]}"; do
        node_ops=$(extract_ops "$SCRIPT_DIR/nodejs_results.txt" "$op")
        dart_ops=$(extract_ops "$SCRIPT_DIR/dart_results.txt" "$op")

        if [ -n "$node_ops" ] && [ -n "$dart_ops" ]; then
            # Calculate ratio (node/dart)
            if [ "$dart_ops" -gt 0 ]; then
                ratio=$(echo "scale=2; $node_ops / $dart_ops" | bc 2>/dev/null || echo "N/A")
            else
                ratio="N/A"
            fi

            printf "%-12s %12s %12s %12s\n" "$op" "$node_ops" "$dart_ops" "${ratio}x"
        elif [ -n "$node_ops" ]; then
            printf "%-12s %12s %12s %12s\n" "$op" "$node_ops" "N/A" "N/A"
        elif [ -n "$dart_ops" ]; then
            printf "%-12s %12s %12s %12s\n" "$op" "N/A" "$dart_ops" "N/A"
        fi
    done

    echo ""
    echo "Note: Ratio shows Node.js/Dart. Values >1 mean Node.js is faster."
    echo "      Values <1 mean Dart is faster."
}

# Main
main() {
    check_dependencies
    build_native_library
    run_nodejs_benchmark
    run_dart_benchmark
    compare_results

    # Cleanup
    rm -f "$SCRIPT_DIR/nodejs_results.txt" "$SCRIPT_DIR/dart_results.txt"
}

main "$@"