#!/usr/bin/env python3
"""
Performance Report Generator

Generates performance trend reports from benchmark results.
Tracks performance over time and identifies new hotspots.
"""

import json
import os
import sys
from datetime import datetime
from pathlib import Path
import statistics

def load_json(filepath):
    """Load JSON file and return contents."""
    try:
        with open(filepath, 'r') as f:
            return json.load(f)
    except FileNotFoundError:
        return None
    except json.JSONDecodeError:
        print(f"Warning: Could not parse {filepath}")
        return None

def extract_metrics(benchmark_data):
    """Extract key metrics from benchmark JSON."""
    if not benchmark_data:
        return None

    metrics = {}

    # Try to extract common metrics
    try:
        if 'operations_per_second' in benchmark_data:
            metrics['ops_per_sec'] = float(benchmark_data['operations_per_second'])

        if 'avg_latency_ns' in benchmark_data:
            metrics['avg_latency_ns'] = float(benchmark_data['avg_latency_ns'])

        if 'p99_latency_ns' in benchmark_data:
            metrics['p99_latency_ns'] = float(benchmark_data['p99_latency_ns'])

        if 'operations_count' in benchmark_data:
            metrics['total_ops'] = int(benchmark_data['operations_count'])

        if 'total_time_ns' in benchmark_data:
            metrics['total_time_ns'] = int(benchmark_data['total_time_ns'])
    except (KeyError, ValueError, TypeError):
        pass

    return metrics if metrics else None

def calculate_trend(historical_metrics):
    """Calculate performance trend from historical data."""
    if len(historical_metrics) < 2:
        return "insufficient_data"

    # Extract ops/sec over time
    ops_values = [m['ops_per_sec'] for m in historical_metrics if 'ops_per_sec' in m]

    if len(ops_values) < 2:
        return "insufficient_data"

    # Calculate trend: comparing last 3 runs to previous 3 runs
    recent = statistics.mean(ops_values[-3:]) if len(ops_values) >= 3 else ops_values[-1]
    previous = statistics.mean(ops_values[-6:-3]) if len(ops_values) >= 6 else ops_values[0]

    if previous == 0:
        return "no_baseline"

    change_pct = ((recent - previous) / previous) * 100

    if change_pct > 5:
        return f"improving (+{change_pct:.1f}%)"
    elif change_pct < -5:
        return f"degrading ({change_pct:.1f}%)"
    else:
        return f"stable ({change_pct:+.1f}%)"

def generate_report(benchmark_dir, output_file):
    """Generate performance report."""
    report = {
        'generated_at': datetime.now().isoformat(),
        'benchmarks': {}
    }

    benchmark_files = {
        'WAL': ('wal_baseline.json', 'wal_current.json'),
        'Sections': ('sections_baseline.json', 'sections_current.json'),
        'Database': ('database_baseline.json', 'database_current.json')
    }

    for name, (baseline_file, current_file) in benchmark_files.items():
        baseline_path = Path(benchmark_dir) / baseline_file
        current_path = Path(benchmark_dir) / current_file

        baseline_data = load_json(baseline_path)
        current_data = load_json(current_path)

        baseline_metrics = extract_metrics(baseline_data)
        current_metrics = extract_metrics(current_data)

        if not current_metrics:
            print(f"Warning: No current data for {name}")
            continue

        benchmark_report = {
            'current': current_metrics,
            'baseline': baseline_metrics,
            'trend': calculate_trend([current_metrics] if current_metrics else [])
        }

        if baseline_metrics and 'ops_per_sec' in current_metrics and 'ops_per_sec' in baseline_metrics:
            change = ((current_metrics['ops_per_sec'] - baseline_metrics['ops_per_sec'])
                      / baseline_metrics['ops_per_sec'] * 100)
            benchmark_report['change_from_baseline_pct'] = change

        report['benchmarks'][name] = benchmark_report

    # Write report
    with open(output_file, 'w') as f:
        json.dump(report, f, indent=2)

    return report

def print_summary(report):
    """Print human-readable summary."""
    print("=" * 70)
    print("WaveDB Performance Report")
    print(f"Generated: {report['generated_at']}")
    print("=" * 70)
    print()

    for name, data in report['benchmarks'].items():
        print(f"\n{name} Benchmark:")
        print("-" * 50)

        if 'current' in data and data['current']:
            current = data['current']
            print(f"  Current Performance:")
            if 'ops_per_sec' in current:
                print(f"    Throughput: {current['ops_per_sec']:,.0f} ops/sec")
            if 'avg_latency_ns' in current:
                print(f"    Avg Latency: {current['avg_latency_ns']:,.0f} ns")
            if 'p99_latency_ns' in current:
                print(f"    P99 Latency: {current['p99_latency_ns']:,.0f} ns")

        if 'baseline' in data and data['baseline']:
            baseline = data['baseline']
            print(f"  Baseline Performance:")
            if 'ops_per_sec' in baseline:
                print(f"    Throughput: {baseline['ops_per_sec']:,.0f} ops/sec")

        if 'change_from_baseline_pct' in data:
            change = data['change_from_baseline_pct']
            if change > 0:
                print(f"  Change: +{change:.1f}% (improvement)")
            elif change < 0:
                print(f"  Change: {change:.1f}% (regression)")
            else:
                print(f"  Change: {change:+.1f}% (stable)")

        if 'trend' in data:
            print(f"  Trend: {data['trend']}")

    print()
    print("=" * 70)

def main():
    """Main entry point."""
    if len(sys.argv) < 2:
        benchmark_dir = ".benchmarks"
        output_file = ".benchmarks/performance_report.json"
    else:
        benchmark_dir = sys.argv[1]
        output_file = sys.argv[2] if len(sys.argv) > 2 else "performance_report.json"

    if not os.path.exists(benchmark_dir):
        print(f"Error: Benchmark directory not found: {benchmark_dir}")
        sys.exit(1)

    report = generate_report(benchmark_dir, output_file)
    print_summary(report)

    print(f"\nReport saved to: {output_file}")

if __name__ == "__main__":
    main()