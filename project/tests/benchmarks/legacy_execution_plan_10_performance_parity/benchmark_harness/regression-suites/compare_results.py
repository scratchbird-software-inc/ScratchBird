#!/usr/bin/env python3
"""
Compare Regression Test Results

Compares test results from original engines vs ScratchBird to:
1. Identify compatibility issues
2. Track progress over time
3. Generate audit reports

Usage:
    python compare_results.py \
        --original=/results/fbt_results_original_20240308.json \
        --scratchbird=/results/fbt_results_scratchbird_20240308.json \
        --output=/results/comparison.html
"""

import argparse
import json
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional, Tuple


@dataclass
class ComparisonResult:
    """Result of comparing original vs ScratchBird test."""
    test_id: str
    original_status: str
    scratchbird_status: str
    comparison: str  # MATCH, REGRESSION, IMPROVEMENT, NEW_PASS, NEW_FAIL
    duration_delta_ms: float
    notes: str = ""


class ResultsComparator:
    """Compares test results between original and ScratchBird."""

    def __init__(self, original_results: Dict, scratchbird_results: Dict):
        self.original = original_results
        self.scratchbird = scratchbird_results
        self.comparisons: List[ComparisonResult] = []

    def compare(self) -> List[ComparisonResult]:
        """Compare all test results."""
        # Build lookup maps
        original_map = {r['test_id']: r for r in self.original.get('results', [])}
        scratchbird_map = {r['test_id']: r for r in self.scratchbird.get('results', [])}

        # Compare all unique test IDs
        all_test_ids = set(original_map.keys()) | set(scratchbird_map.keys())

        for test_id in sorted(all_test_ids):
            orig = original_map.get(test_id, {'status': 'MISSING', 'duration_ms': 0})
            sb = scratchbird_map.get(test_id, {'status': 'MISSING', 'duration_ms': 0})

            comparison = self._classify_comparison(
                test_id,
                orig.get('status', 'UNKNOWN'),
                sb.get('status', 'UNKNOWN'),
                orig.get('duration_ms', 0),
                sb.get('duration_ms', 0)
            )

            self.comparisons.append(comparison)

        return self.comparisons

    def _classify_comparison(self, test_id: str, orig_status: str,
                             sb_status: str, orig_duration: float,
                             sb_duration: float) -> ComparisonResult:
        """Classify the comparison result."""
        duration_delta = sb_duration - orig_duration

        # Define status hierarchy
        success_statuses = {'PASS', 'PASS_EQUIVALENT'}
        failure_statuses = {'FAIL', 'ERROR'}

        orig_pass = orig_status in success_statuses
        sb_pass = sb_status in success_statuses

        if orig_status == 'MISSING':
            comparison = 'NEW_TEST'
            notes = 'Test exists only in ScratchBird run'
        elif sb_status == 'MISSING':
            comparison = 'MISSING_TEST'
            notes = 'Test was not run against ScratchBird'
        elif orig_pass and sb_pass:
            comparison = 'MATCH'
            notes = 'Both engines pass'
        elif not orig_pass and not sb_pass:
            comparison = 'MATCH_FAIL'
            notes = 'Both engines fail (expected behavior may differ)'
        elif orig_pass and not sb_pass:
            comparison = 'REGRESSION'
            notes = f'Original passes, ScratchBird {sb_status.lower()}'
        elif not orig_pass and sb_pass:
            comparison = 'IMPROVEMENT'
            notes = 'Original fails, ScratchBird passes'
        else:
            comparison = 'UNKNOWN'
            notes = 'Unexpected status combination'

        return ComparisonResult(
            test_id=test_id,
            original_status=orig_status,
            scratchbird_status=sb_status,
            comparison=comparison,
            duration_delta_ms=duration_delta,
            notes=notes
        )

    def generate_summary(self) -> Dict:
        """Generate summary statistics."""
        counts = {
            'MATCH': 0,
            'MATCH_FAIL': 0,
            'REGRESSION': 0,
            'IMPROVEMENT': 0,
            'NEW_TEST': 0,
            'MISSING_TEST': 0,
            'UNKNOWN': 0,
        }

        for comp in self.comparisons:
            counts[comp.comparison] = counts.get(comp.comparison, 0) + 1

        total = len(self.comparisons)
        success_rate = (counts['MATCH'] + counts['IMPROVEMENT']) / total * 100 if total > 0 else 0
        regression_rate = counts['REGRESSION'] / total * 100 if total > 0 else 0

        return {
            'total_tests': total,
            'match': counts['MATCH'],
            'match_fail': counts['MATCH_FAIL'],
            'regressions': counts['REGRESSION'],
            'improvements': counts['IMPROVEMENT'],
            'new_tests': counts['NEW_TEST'],
            'missing_tests': counts['MISSING_TEST'],
            'success_rate': success_rate,
            'regression_rate': regression_rate,
        }

    def generate_html_report(self) -> str:
        """Generate HTML comparison report."""
        summary = self.generate_summary()

        html = f"""<!DOCTYPE html>
<html>
<head>
    <title>ScratchBird Regression Test Comparison</title>
    <style>
        body {{ font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; margin: 40px; }}
        h1 {{ color: #333; }}
        h2 {{ color: #555; border-bottom: 2px solid #ddd; padding-bottom: 8px; }}
        .summary {{ background: #f5f5f5; padding: 20px; border-radius: 8px; margin: 20px 0; }}
        .metric {{ display: inline-block; margin: 10px 20px; }}
        .metric-value {{ font-size: 32px; font-weight: bold; color: #2196F3; }}
        .metric-label {{ font-size: 14px; color: #666; }}
        .success {{ color: #4CAF50; }}
        .warning {{ color: #FF9800; }}
        .error {{ color: #F44336; }}
        table {{ width: 100%; border-collapse: collapse; margin: 20px 0; }}
        th {{ background: #2196F3; color: white; padding: 12px; text-align: left; }}
        td {{ padding: 10px; border-bottom: 1px solid #ddd; }}
        tr:hover {{ background: #f5f5f5; }}
        .comparison-MATCH {{ background: #E8F5E9; }}
        .comparison-REGRESSION {{ background: #FFEBEE; }}
        .comparison-IMPROVEMENT {{ background: #E3F2FD; }}
        .metadata {{ font-size: 12px; color: #666; margin: 20px 0; }}
    </style>
</head>
<body>
    <h1>🐦 ScratchBird Regression Test Comparison</h1>

    <div class="metadata">
        <p><strong>Generated:</strong> {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}</p>
        <p><strong>Original Engine:</strong> {self.original.get('metadata', {}).get('target', 'unknown')}
           ({self.original.get('metadata', {}).get('timestamp', 'unknown')})</p>
        <p><strong>ScratchBird Mode:</strong> {self.scratchbird.get('metadata', {}).get('mode', 'unknown')}
           ({self.scratchbird.get('metadata', {}).get('timestamp', 'unknown')})</p>
    </div>

    <div class="summary">
        <h2>Summary</h2>
        <div class="metric">
            <div class="metric-value {'success' if summary['success_rate'] >= 90 else 'warning' if summary['success_rate'] >= 70 else 'error'}">
                {summary['success_rate']:.1f}%
            </div>
            <div class="metric-label">Success Rate</div>
        </div>
        <div class="metric">
            <div class="metric-value success">{summary['match']}</div>
            <div class="metric-label">Matches</div>
        </div>
        <div class="metric">
            <div class="metric-value {'success' if summary['regressions'] == 0 else 'error'}">{summary['regressions']}</div>
            <div class="metric-label">Regressions ⚠️</div>
        </div>
        <div class="metric">
            <div class="metric-value">{summary['improvements']}</div>
            <div class="metric-label">Improvements</div>
        </div>
        <div class="metric">
            <div class="metric-value">{summary['total_tests']}</div>
            <div class="metric-label">Total Tests</div>
        </div>
    </div>

    <h2>Detailed Results</h2>
    <table>
        <thead>
            <tr>
                <th>Test ID</th>
                <th>Original</th>
                <th>ScratchBird</th>
                <th>Comparison</th>
                <th>Duration Δ</th>
                <th>Notes</th>
            </tr>
        </thead>
        <tbody>
"""

        for comp in self.comparisons:
            duration_class = ''
            duration_str = f"{comp.duration_delta_ms:+.0f}ms"
            if comp.duration_delta_ms > 1000:
                duration_class = 'warning'
            elif comp.duration_delta_ms < -500:
                duration_str += ' 🚀'

            html += f"""
            <tr class="comparison-{comp.comparison}">
                <td><code>{comp.test_id}</code></td>
                <td>{comp.original_status}</td>
                <td>{comp.scratchbird_status}</td>
                <td><strong>{comp.comparison}</strong></td>
                <td class="{duration_class}">{duration_str}</td>
                <td>{comp.notes}</td>
            </tr>
"""

        html += """
        </tbody>
    </table>

    <h2>Legend</h2>
    <ul>
        <li><strong>MATCH</strong> - Both engines pass (identical behavior)</li>
        <li><strong>MATCH_FAIL</strong> - Both engines fail (may be acceptable difference)</li>
        <li><strong>REGRESSION</strong> - Original passes, ScratchBird fails (needs investigation)</li>
        <li><strong>IMPROVEMENT</strong> - Original fails, ScratchBird passes (unexpected!)</li>
        <li><strong>NEW_TEST</strong> - Test only in ScratchBird run</li>
        <li><strong>MISSING_TEST</strong> - Test missing from ScratchBird run</li>
    </ul>

    <footer style="margin-top: 40px; padding-top: 20px; border-top: 1px solid #ddd; color: #666; font-size: 12px;">
        <p>Generated by ScratchBird Benchmark Suite</p>
    </footer>
</body>
</html>
"""
        return html

    def generate_markdown_report(self) -> str:
        """Generate Markdown comparison report."""
        summary = self.generate_summary()

        md = f"""# ScratchBird Regression Test Comparison

**Generated:** {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}

## Summary

| Metric | Value |
|--------|-------|
| Success Rate | {summary['success_rate']:.1f}% |
| Matches | {summary['match']} |
| Regressions | {summary['regressions']} ⚠️ |
| Improvements | {summary['improvements']} |
| Total Tests | {summary['total_tests']} |

## Detailed Results

| Test ID | Original | ScratchBird | Comparison | Notes |
|---------|----------|-------------|------------|-------|
"""

        for comp in self.comparisons:
            md += f"| {comp.test_id} | {comp.original_status} | {comp.scratchbird_status} | **{comp.comparison}** | {comp.notes} |\n"

        md += """
## Legend

- **MATCH** - Both engines pass (identical behavior)
- **MATCH_FAIL** - Both engines fail (may be acceptable difference)
- **REGRESSION** - Original passes, ScratchBird fails (needs investigation)
- **IMPROVEMENT** - Original fails, ScratchBird passes (unexpected!)
- **NEW_TEST** - Test only in ScratchBird run
- **MISSING_TEST** - Test missing from ScratchBird run

---
Generated by ScratchBird Benchmark Suite
"""
        return md


def main():
    parser = argparse.ArgumentParser(description='Compare Regression Test Results')
    parser.add_argument('--original', type=Path, required=True,
                        help='Original engine results JSON file')
    parser.add_argument('--scratchbird', type=Path, required=True,
                        help='ScratchBird results JSON file')
    parser.add_argument('--output', type=Path, required=True,
                        help='Output file (HTML or Markdown)')

    args = parser.parse_args()

    # Load results
    original_data = json.loads(args.original.read_text())
    scratchbird_data = json.loads(args.scratchbird.read_text())

    # Compare
    comparator = ResultsComparator(original_data, scratchbird_data)
    comparisons = comparator.compare()

    # Generate report
    if args.output.suffix == '.html':
        report = comparator.generate_html_report()
    else:
        report = comparator.generate_markdown_report()

    args.output.write_text(report)

    # Print summary
    summary = comparator.generate_summary()
    print(f"\n{'='*60}")
    print(f"Comparison Summary")
    print(f"{'='*60}")
    print(f"Total Tests: {summary['total_tests']}")
    print(f"Success Rate: {summary['success_rate']:.1f}%")
    print(f"Matches: {summary['match']}")
    print(f"Regressions: {summary['regressions']}")
    print(f"Improvements: {summary['improvements']}")
    print(f"\nReport saved to: {args.output}")


if __name__ == '__main__':
    main()
