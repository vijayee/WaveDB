#!/bin/bash
#
# Performance Regression Detection Script
#
# This script runs all benchmarks and compares them against baseline measurements.
# It fails if performance degrades by more than 10% from baseline.
#

set -e

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_ROOT/build"
BASELINE_DIR="$PROJECT_ROOT/.benchmarks"
DEGRADATION_THRESHOLD=10  # Percent

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "=== WaveDB Performance Regression Detection ==="
echo ""

# Ensure build directory exists
if [ ! -d "$BUILD_DIR" ]; then
    echo -e "${RED}Error: Build directory not found at $BUILD_DIR${NC}"
    echo "Please build with: cmake .. && make benchmark"
    exit 1
fi

# Ensure baseline directory exists
mkdir -p "$BASELINE_DIR"

# Check if benchmarks exist
BENCHMARK_WAL="$BUILD_DIR/benchmark_wal"
BENCHMARK_SECTIONS="$BUILD_DIR/benchmark_sections"
BENCHMARK_DATABASE="$BUILD_DIR/benchmark_database"

if [ ! -f "$BENCHMARK_WAL" ] || [ ! -f "$BENCHMARK_SECTIONS" ] || [ ! -f "$BENCHMARK_DATABASE" ]; then
    echo -e "${RED}Error: Benchmark executables not found${NC}"
    echo "Please build with: cmake .. -DBUILD_BENCHMARKS=ON && make benchmark"
    exit 1
fi

# Function to parse benchmark output and extract metrics
parse_benchmark() {
    local output_file="$1"
    local metric_name="$2"

    # Extract value from benchmark JSON output
    # Format: "metric_name": value
    grep "\"$metric_name\"" "$output_file" | head -1 | sed 's/.*: \([0-9.]*\).*/\1/'
}

# Function to compare current vs baseline
compare_performance() {
    local benchmark_name="$1"
    local current_file="$2"
    local baseline_file="$3"

    if [ ! -f "$baseline_file" ]; then
        echo -e "${YELLOW}No baseline found for $benchmark_name, creating new baseline${NC}"
        cp "$current_file" "$baseline_file"
        return 0
    fi

    # Extract key metrics
    local current_ops=$(parse_benchmark "$current_file" "operations_per_second")
    local baseline_ops=$(parse_benchmark "$baseline_file" "operations_per_second")

    if [ -z "$current_ops" ] || [ -z "$baseline_ops" ]; then
        echo -e "${YELLOW}Warning: Could not parse metrics for $benchmark_name${NC}"
        return 0
    fi

    # Calculate percentage change
    local change=$(echo "scale=2; (($current_ops - $baseline_ops) / $baseline_ops) * 100" | bc)

    echo ""
    echo "=== $benchmark_name ==="
    echo "Current throughput: $current_ops ops/sec"
    echo "Baseline throughput: $baseline_ops ops/sec"

    if (( $(echo "$change < -$DEGRADATION_THRESHOLD" | bc -l) )); then
        echo -e "${RED}FAIL: Performance degraded by ${change#-}% (threshold: ${DEGRADATION_THRESHOLD}%)${NC}"
        return 1
    elif (( $(echo "$change > 0" | bc -l) )); then
        echo -e "${GREEN}PASS: Performance improved by ${change}%${NC}"
    else
        echo -e "${GREEN}PASS: Performance within threshold (${change#-}% degradation)${NC}"
    fi

    return 0
}

# Run benchmarks
echo "Running benchmarks..."
echo ""

# Track overall status
OVERALL_STATUS=0

# WAL benchmark
echo "--- WAL Benchmark ---"
CURRENT_WAL="$BASELINE_DIR/wal_current.json"
BASELINE_WAL="$BASELINE_DIR/wal_baseline.json"

if [ ! -f "$BASELINE_WAL" ]; then
    echo "No baseline found, running benchmark to establish baseline..."
fi

"$BENCHMARK_WAL" --output="$CURRENT_WAL" --benchmark_format=json || {
    echo -e "${RED}WAL benchmark failed to run${NC}"
    OVERALL_STATUS=1
}

if [ -f "$CURRENT_WAL" ]; then
    compare_performance "WAL" "$CURRENT_WAL" "$BASELINE_WAL" || OVERALL_STATUS=1
fi

echo ""

# Sections benchmark
echo "--- Sections Benchmark ---"
CURRENT_SECTIONS="$BASELINE_DIR/sections_current.json"
BASELINE_SECTIONS="$BASELINE_DIR/sections_baseline.json"

if [ ! -f "$BASELINE_SECTIONS" ]; then
    echo "No baseline found, running benchmark to establish baseline..."
fi

"$BENCHMARK_SECTIONS" --output="$CURRENT_SECTIONS" --benchmark_format=json || {
    echo -e "${RED}Sections benchmark failed to run${NC}"
    OVERALL_STATUS=1
}

if [ -f "$CURRENT_SECTIONS" ]; then
    compare_performance "Sections" "$CURRENT_SECTIONS" "$BASELINE_SECTIONS" || OVERALL_STATUS=1
fi

echo ""

# Database benchmark
echo "--- Database Benchmark ---"
CURRENT_DATABASE="$BASELINE_DIR/database_current.json"
BASELINE_DATABASE="$BASELINE_DIR/database_baseline.json"

if [ ! -f "$BASELINE_DATABASE" ]; then
    echo "No baseline found, running benchmark to establish baseline..."
fi

"$BENCHMARK_DATABASE" --output="$CURRENT_DATABASE" --benchmark_format=json || {
    echo -e "${RED}Database benchmark failed to run${NC}"
    OVERALL_STATUS=1
}

if [ -f "$CURRENT_DATABASE" ]; then
    compare_performance "Database" "$CURRENT_DATABASE" "$BASELINE_DATABASE" || OVERALL_STATUS=1
fi

echo ""
echo "================================"

if [ $OVERALL_STATUS -eq 0 ]; then
    echo -e "${GREEN}All performance checks passed!${NC}"
    echo ""
    echo "To update baselines, run:"
    echo "  cp $BASELINE_DIR/*_current.json $BASELINE_DIR/*_baseline.json"
    exit 0
else
    echo -e "${RED}Performance regression detected!${NC}"
    echo ""
    echo "To investigate, compare:"
    echo "  - $BASELINE_DIR/*_current.json"
    echo "  - $BASELINE_DIR/*_baseline.json"
    echo ""
    echo "To accept new baselines, run:"
    echo "  cp $BASELINE_DIR/*_current.json $BASELINE_DIR/*_baseline.json"
    exit 1
fi