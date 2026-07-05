#!/usr/bin/env python3
"""
Compare Stress Test Results Across Engines

Generates HTML/Markdown reports comparing performance metrics
between Firebird, MySQL, PostgreSQL, and ScratchBird.
"""

import argparse
import json
import sys
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional

import matplotlib.pyplot as plt
import matplotlib
matplotlib.use('Agg')  # Non-interactive backend

PROJECT_ROOT = Path(__file__).resolve().parents[2]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from benchmark_provenance import validate_scratchbird_result_provenance


@dataclass
class StressResult:
    """Parsed stress test result."""
    engine: str
    timestamp: str
    data_loading: List[Dict]
    test_results: List[Dict]
    summary: Dict


def load_results(file_path: Path) -> Optional[StressResult]:
    """Load results from JSON file."""
    try:
        validate_scratchbird_result_provenance(file_path)
        data = json.loads(file_path.read_text())
        return StressResult(
            engine=data['metadata']['engine'],
            timestamp=data['metadata']['timestamp'],
            data_loading=data.get('data_loading', []),
            test_results=data.get('test_results', []),
            summary=data.get('summary', {})
        )
    except Exception as e:
        print(f"Error loading {file_path}: {e}")
        return None


def generate_data_loading_chart(results: List[StressResult], output_dir: Path):
    """Generate chart comparing data loading speeds."""
    fig, axes = plt.subplots(2, 2, figsize=(14, 10))
    fig.suptitle('Data Loading Performance Comparison', fontsize=16)

    tables = ['customers', 'products', 'orders', 'order_items']

    for idx, table in enumerate(tables):
        ax = axes[idx // 2, idx % 2]

        engines = []
        rows_per_sec = []
        durations = []

        for result in results:
            for dl in result.data_loading:
                if dl['table_name'] == table:
                    engines.append(result.engine)
                    rows_per_sec.append(dl.get('rows_per_second', 0))
                    durations.append(dl['duration_ms'] / 1000)
                    break

        # Bar chart of rows/second
        x = range(len(engines))
        ax.bar(x, rows_per_sec, color=['#e74c3c', '#3498db', '#2ecc71'][:len(engines)])
        ax.set_xlabel('Engine')
        ax.set_ylabel('Rows/Second')
        ax.set_title(f'{table.capitalize()} Loading Speed')
        ax.set_xticks(x)
        ax.set_xticklabels(engines)

        # Add value labels on bars
        for i, (rps, dur) in enumerate(zip(rows_per_sec, durations)):
            ax.text(i, rps, f'{rps:,.0f}\n({dur:.1f}s)',
                   ha='center', va='bottom', fontsize=8)

    plt.tight_layout()
    plt.savefig(output_dir / 'data_loading_comparison.png', dpi=150)
    plt.close()


def generate_join_performance_chart(results: List[StressResult], output_dir: Path):
    """Generate chart comparing JOIN test performance."""
    # Collect all test names
    all_tests = set()
    for result in results:
        for test in result.test_results:
            all_tests.add(test['test_name'])

    # Filter to JOIN tests only
    join_tests = sorted([t for t in all_tests if any(x in t.lower()
                         for x in ['join', 'inner', 'outer', 'left', 'right', 'cross', 'self'])])

    if len(join_tests) < 3:
        return  # Not enough data

    # Select top 10 longest tests for comparison
    test_durations = {}
    for test_name in join_tests:
        for result in results:
            for test in result.test_results:
                if test['test_name'] == test_name and test['status'] == 'passed':
                    if test_name not in test_durations:
                        test_durations[test_name] = {}
                    test_durations[test_name][result.engine] = test['duration_ms']

    # Take top 10 by average duration
    avg_durations = {
        name: sum(durations.values()) / len(durations)
        for name, durations in test_durations.items()
    }
    top_tests = sorted(avg_durations.keys(), key=lambda x: avg_durations[x], reverse=True)[:10]

    fig, ax = plt.subplots(figsize=(14, 8))

    engines = [r.engine for r in results]
    x = range(len(top_tests))
    width = 0.25

    colors = {'firebird': '#e74c3c', 'mysql': '#3498db', 'postgresql': '#2ecc71', 'scratchbird': '#9b59b6'}

    for i, engine in enumerate(engines):
        durations = []
        for test_name in top_tests:
            duration = test_durations.get(test_name, {}).get(engine, 0)
            durations.append(duration / 1000)  # Convert to seconds

        offset = (i - len(engines)/2) * width + width/2
        ax.bar([xi + offset for xi in x], durations, width,
               label=engine, color=colors.get(engine, '#95a5a6'))

    ax.set_xlabel('Test Name')
    ax.set_ylabel('Duration (seconds)')
    ax.set_title('JOIN Test Performance Comparison')
    ax.set_xticks(x)
    ax.set_xticklabels([t[:30] + '...' if len(t) > 30 else t for t in top_tests],
                       rotation=45, ha='right')
    ax.legend()
    ax.grid(axis='y', alpha=0.3)

    plt.tight_layout()
    plt.savefig(output_dir / 'join_performance_comparison.png', dpi=150)
    plt.close()


def generate_summary_table(results: List[StressResult]) -> str:
    """Generate summary table in Markdown format."""
    md = "## Performance Summary\n\n"

    # Data loading summary
    md += "### Data Loading Performance\n\n"
    md += "| Engine | Total Rows | Total Time | Avg Rows/sec |\n"
    md += "|--------|-----------|------------|-------------|\n"

    for result in results:
        total_rows = sum(dl['row_count'] for dl in result.data_loading)
        total_time = sum(dl['duration_ms'] for dl in result.data_loading) / 1000
        avg_rate = total_rows / total_time if total_time > 0 else 0
        md += f"| {result.engine} | {total_rows:,} | {total_time:.1f}s | {avg_rate:,.0f} |\n"

    # Test results summary
    md += "\n### Test Results Summary\n\n"
    md += "| Engine | Total Tests | Passed | Failed | Errors | Avg Time |\n"
    md += "|--------|-------------|--------|--------|--------|----------|\n"

    for result in results:
        total = result.summary.get('total_tests', 0)
        passed = result.summary.get('passed', 0)
        failed = result.summary.get('failed', 0)
        errors = result.summary.get('errors', 0)
        total_time = result.summary.get('total_duration_ms', 0) / 1000
        avg_time = total_time / total if total > 0 else 0
        md += f"| {result.engine} | {total} | {passed} | {failed} | {errors} | {avg_time:.2f}s |\n"

    return md


def generate_html_report(results: List[StressResult], output_file: Path):
    """Generate comprehensive HTML report."""

    # Generate charts
    generate_data_loading_chart(results, output_file.parent)
    generate_join_performance_chart(results, output_file.parent)

    # Get summary data
    summary_md = generate_summary_table(results)

    html = f"""<!DOCTYPE html>
<html>
<head>
    <title>ScratchBird Stress Test Comparison</title>
    <style>
        body {{
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            margin: 40px;
            background: #f5f5f5;
        }}
        .container {{
            max-width: 1400px;
            margin: 0 auto;
            background: white;
            padding: 40px;
            border-radius: 8px;
            box-shadow: 0 2px 4px rgba(0,0,0,0.1);
        }}
        h1 {{
            color: #2c3e50;
            border-bottom: 3px solid #3498db;
            padding-bottom: 15px;
        }}
        h2 {{
            color: #34495e;
            margin-top: 40px;
            border-bottom: 2px solid #ecf0f1;
            padding-bottom: 10px;
        }}
        h3 {{
            color: #7f8c8d;
            margin-top: 30px;
        }}
        table {{
            width: 100%;
            border-collapse: collapse;
            margin: 20px 0;
        }}
        th {{
            background: #3498db;
            color: white;
            padding: 12px;
            text-align: left;
        }}
        td {{
            padding: 10px;
            border-bottom: 1px solid #ecf0f1;
        }}
        tr:hover {{
            background: #f8f9fa;
        }}
        .metric {{
            display: inline-block;
            margin: 10px 20px;
            padding: 15px 20px;
            background: #ecf0f1;
            border-radius: 8px;
        }}
        .metric-value {{
            font-size: 28px;
            font-weight: bold;
            color: #2c3e50;
        }}
        .metric-label {{
            font-size: 12px;
            color: #7f8c8d;
            text-transform: uppercase;
        }}
        .engine-firebird {{ color: #e74c3c; }}
        .engine-mysql {{ color: #3498db; }}
        .engine-postgresql {{ color: #2ecc71; }}
        .engine-scratchbird {{ color: #9b59b6; }}
        .chart-container {{
            margin: 30px 0;
            text-align: center;
        }}
        .chart-container img {{
            max-width: 100%;
            border: 1px solid #ecf0f1;
            border-radius: 4px;
        }}
        .pass {{ color: #27ae60; font-weight: bold; }}
        .fail {{ color: #e74c3c; font-weight: bold; }}
        .error {{ color: #f39c12; font-weight: bold; }}
        .test-details {{
            font-size: 12px;
            color: #7f8c8d;
        }}
    </style>
</head>
<body>
    <div class="container">
        <h1>🐦 ScratchBird Stress Test Comparison</h1>

        <div class="metadata">
            <p><strong>Generated:</strong> {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}</p>
            <p><strong>Engines Tested:</strong> {', '.join(r.engine for r in results)}</p>
        </div>

        <h2>Executive Summary</h2>
        <div class="summary-metrics">
"""

    # Add summary metrics
    for result in results:
        total_tests = result.summary.get('total_tests', 0)
        passed = result.summary.get('passed', 0)
        total_rows = sum(dl['row_count'] for dl in result.data_loading)
        total_time = result.summary.get('total_duration_ms', 0) / 1000

        html += f"""
            <div class="metric">
                <div class="metric-value engine-{result.engine}">{passed}/{total_tests}</div>
                <div class="metric-label">{result.engine.title()} Tests Passed</div>
            </div>
        """

    html += """
        </div>

        <h2>Data Loading Performance</h2>
        <div class="chart-container">
            <img src="data_loading_comparison.png" alt="Data Loading Comparison">
        </div>

        <h2>JOIN Test Performance</h2>
        <div class="chart-container">
            <img src="join_performance_comparison.png" alt="JOIN Performance Comparison">
        </div>

        <h2>Detailed Results</h2>
"""

    # Add test details table
    html += """
        <table>
            <thead>
                <tr>
                    <th>Test Name</th>
"""
    for result in results:
        html += f"<th>{result.engine.title()}</th>"
    html += """
                </tr>
            </thead>
            <tbody>
"""

    # Get all test names
    all_tests = set()
    for result in results:
        for test in result.test_results:
            all_tests.add(test['test_name'])

    for test_name in sorted(all_tests):
        html += f"<tr><td><code>{test_name}</code></td>"

        for result in results:
            test_result = next((t for t in result.test_results if t['test_name'] == test_name), None)
            if test_result:
                status = test_result['status']
                duration = test_result['duration_ms'] / 1000
                css_class = 'pass' if status == 'passed' else 'fail' if status == 'failed' else 'error'
                html += f'<td class="{css_class}">{status}<br><span class="test-details">{duration:.2f}s</span></td>'
            else:
                html += '<td>-</td>'

        html += '</tr>'

    html += """
            </tbody>
        </table>

        <footer style="margin-top: 40px; padding-top: 20px; border-top: 1px solid #ecf0f1; color: #7f8c8d; font-size: 12px;">
            <p>Generated by ScratchBird Stress Test Suite</p>
        </footer>
    </div>
</body>
</html>
"""

    output_file.write_text(html)
    print(f"HTML report generated: {output_file}")


def main():
    parser = argparse.ArgumentParser(description='Compare Stress Test Results')
    parser.add_argument('--results-dir', type=Path, required=True,
                        help='Directory containing result JSON files')
    parser.add_argument('--output', type=Path, default=Path('comparison.html'),
                        help='Output HTML file')

    args = parser.parse_args()

    # Load all results
    results = []
    for json_file in args.results_dir.glob('*.json'):
        result = load_results(json_file)
        if result:
            results.append(result)

    if not results:
        print("No valid result files found!")
        return

    print(f"Loaded {len(results)} result files")

    # Generate report
    generate_html_report(results, args.output)

    # Print summary
    print("\n" + "="*60)
    print("Summary")
    print("="*60)
    for result in results:
        total = result.summary.get('total_tests', 0)
        passed = result.summary.get('passed', 0)
        total_rows = sum(dl['row_count'] for dl in result.data_loading)
        print(f"\n{result.engine.upper()}:")
        print(f"  Data loaded: {total_rows:,} rows")
        print(f"  Tests: {passed}/{total} passed")


if __name__ == '__main__':
    main()
