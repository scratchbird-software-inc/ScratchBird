#!/usr/bin/env python3
"""
Benchmark Result Formatter

Generates human-readable text reports from benchmark results.
These reports can be:
- Reviewed before sharing
- Posted to GitHub issues/discussions
- Emailed to project maintainers
- Stored for later comparison

No external servers involved - completely offline and transparent.
"""

import json
import platform
import sys
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, List, Optional

PROJECT_ROOT = Path(__file__).resolve().parents[2]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from benchmark_provenance import validate_scratchbird_result_provenance


@dataclass
class FormattedReport:
    """A formatted benchmark report."""
    title: str
    content: str
    filepath: Path


class TextReportFormatter:
    """Formats benchmark results as human-readable text."""

    def __init__(self):
        pass

    def format_single_result(
        self,
        benchmark_file: Path,
        system_info_file: Optional[Path] = None,
        tags: Optional[List[str]] = None,
        notes: Optional[str] = None
    ) -> FormattedReport:
        """
        Format a single benchmark result as text.

        Args:
            benchmark_file: Path to benchmark JSON result
            system_info_file: Path to system info JSON (optional)
            tags: Tags for categorization
            notes: Additional notes

        Returns:
            FormattedReport with title, content, and suggested filepath
        """
        # Load benchmark results
        with open(benchmark_file, 'r') as f:
            benchmark = json.load(f)
        scratchbird_provenance = validate_scratchbird_result_provenance(benchmark_file)

        # Load system info if provided
        system_info = None
        if system_info_file and system_info_file.exists():
            with open(system_info_file, 'r') as f:
                system_info = json.load(f)

        # Build report
        lines = []

        # Header
        lines.append("=" * 70)
        lines.append("SCRATCHBIRD BENCHMARK REPORT")
        lines.append("=" * 70)
        lines.append("")

        # Metadata
        metadata = benchmark.get('metadata', {})
        lines.append("BENCHMARK METADATA")
        lines.append("-" * 70)
        lines.append(f"Engine Tested:      {metadata.get('engine', 'Unknown')}")
        lines.append(f"Test Suite:         {metadata.get('suite', 'Unknown')}")
        lines.append(f"Timestamp:          {metadata.get('timestamp', datetime.now().isoformat())}")
        if scratchbird_provenance:
            runtime = scratchbird_provenance["scratchbird_runtime"]
            binary = runtime["server_binary"]
            repo = runtime["build_identity"]["scratchbird_repo"]
            lines.append(f"ScratchBird Binary: {binary['realpath']}")
            lines.append(f"Binary SHA256:      {binary['sha256']}")
            lines.append(f"ScratchBird HEAD:   {repo.get('git_head', 'Unknown')}")
            lines.append(f"Build Dir:          {runtime['build_identity'].get('build_dir', 'Unknown')}")
        if tags:
            lines.append(f"Tags:               {', '.join(tags)}")
        if notes:
            lines.append(f"Notes:              {notes}")
        lines.append("")

        # System Information
        if system_info:
            lines.append("SYSTEM INFORMATION")
            lines.append("-" * 70)

            cpu = system_info.get('cpu', {})
            lines.append(f"CPU:                {cpu.get('model', 'Unknown')}")
            lines.append(f"  Vendor:           {cpu.get('vendor', 'Unknown')}")
            lines.append(f"  Physical Cores:   {cpu.get('physical_cores', 'Unknown')}")
            lines.append(f"  Logical Cores:    {cpu.get('logical_cores', 'Unknown')}")
            lines.append(f"  Frequency:        {cpu.get('base_frequency_mhz', 'Unknown')} MHz (base)")
            lines.append(f"  Virtualization:   {cpu.get('virtualization', 'Unknown')}")
            lines.append("")

            memory = system_info.get('memory', {})
            total_gb = memory.get('total_mb', 0) / 1024
            lines.append(f"Memory:             {total_gb:.1f} GB total")
            lines.append(f"  Type:             {memory.get('type', 'Unknown')} @ {memory.get('speed_mhz', 'Unknown')} MHz")
            lines.append(f"  Used:             {memory.get('percent_used', 'Unknown')}%")
            lines.append("")

            os_info = system_info.get('os', {})
            lines.append(f"Operating System:   {os_info.get('distribution', os_info.get('name', 'Unknown'))}")
            lines.append(f"  Version:          {os_info.get('version', 'Unknown')}")
            lines.append(f"  Kernel:           {os_info.get('kernel', 'Unknown')}")
            lines.append(f"  Architecture:     {os_info.get('architecture', 'Unknown')}")
            lines.append("")

            disks = system_info.get('disks', [])
            if disks:
                lines.append("Storage:")
                for disk in disks:
                    lines.append(f"  {disk.get('device', 'Unknown')}:")
                    lines.append(f"    Type:           {disk.get('type', 'Unknown')}")
                    lines.append(f"    Filesystem:     {disk.get('filesystem', 'Unknown')}")
                    lines.append(f"    Total:          {disk.get('total_gb', 0):.1f} GB")
                    lines.append(f"    Free:           {disk.get('free_gb', 0):.1f} GB ({100 - disk.get('percent_used', 0):.1f}%)")
            lines.append("")

            container = system_info.get('container', {})
            if container.get('is_container'):
                lines.append(f"Container:          {container.get('container_type', 'Unknown')}")
            elif container.get('is_vm'):
                lines.append(f"Virtual Machine:    {container.get('vm_hypervisor', 'Unknown')}")
            else:
                lines.append("Environment:        Bare metal")
            lines.append("")

        # Test Results
        lines.append("TEST RESULTS")
        lines.append("-" * 70)

        summary = benchmark.get('summary', {})
        lines.append(f"Total Tests:        {summary.get('total_tests', 'Unknown')}")
        lines.append(f"Passed:             {summary.get('passed', 'Unknown')}")
        lines.append(f"Failed:             {summary.get('failed', 'Unknown')}")
        lines.append(f"Errors:             {summary.get('errors', 'Unknown')}")
        if 'unsupported' in summary:
            lines.append(f"Unsupported:        {summary.get('unsupported', 'Unknown')}")
        if 'plan_capture_success' in summary:
            lines.append(f"Plan Capture OK:    {summary.get('plan_capture_success', 'Unknown')}")
        lines.append(f"Score:              {summary.get('score', 'Unknown')}")
        lines.append("")

        # Detailed results by category
        results = benchmark.get('results', {})
        if results:
            lines.append("RESULTS BY CATEGORY")
            lines.append("-" * 70)

            for category, cat_results in results.items():
                lines.append(f"")
                lines.append(f"{category.upper()}")

                if isinstance(cat_results, dict):
                    if 'passed' in cat_results and 'total' in cat_results:
                        lines.append(f"  Passed: {cat_results['passed']}/{cat_results['total']}")
                    elif 'tests' in cat_results:
                        for test in cat_results['tests']:
                            test_name = test.get('test_name', test.get('name', 'Unknown'))
                            status = test.get('status', 'Unknown')
                            symbol = "✓" if status == 'passed' else "✗"
                            lines.append(f"  {symbol} {test_name}: {status}")
                            if 'duration_ms' in test:
                                lines.append(f"      Duration: {test['duration_ms']:.2f}ms")
                            if test.get('plan_expectation_status'):
                                lines.append(f"      Plan: {test['plan_expectation_status']}")
                            if test.get('comparative_verdict'):
                                lines.append(f"      Verdict: {test['comparative_verdict']}")
                            if test.get('error_message'):
                                lines.append(f"      Error: {test['error_message'][:100]}")
            lines.append("")

        # Footer
        lines.append("=" * 70)
        lines.append("END OF REPORT")
        lines.append("=" * 70)
        lines.append("")
        lines.append("This report was generated by ScratchBird Benchmark Suite.")
        lines.append("To submit: Copy this content to a GitHub issue or discussion.")
        lines.append("")

        content = "\n".join(lines)

        # Generate filename
        engine = metadata.get('engine', 'unknown')
        suite = metadata.get('suite', 'test')
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        filename = f"benchmark_report_{engine}_{suite}_{timestamp}.txt"
        filepath = benchmark_file.parent / filename

        title = f"Benchmark Report: {engine.upper()} - {suite.upper()}"

        return FormattedReport(title=title, content=content, filepath=filepath)

    def format_comparison(
        self,
        result_files: List[Path],
        system_info_file: Optional[Path] = None
    ) -> FormattedReport:
        """
        Format a comparison of multiple benchmark results.

        Args:
            result_files: List of benchmark result files to compare
            system_info_file: System info file (shared across results)

        Returns:
            FormattedReport with comparison content
        """
        # Load all results
        all_results = []
        for f in result_files:
            validate_scratchbird_result_provenance(f)
            with open(f, 'r') as fp:
                all_results.append(json.load(fp))

        # Load system info
        system_info = None
        if system_info_file and system_info_file.exists():
            with open(system_info_file, 'r') as f:
                system_info = json.load(f)

        lines = []

        # Header
        lines.append("=" * 70)
        lines.append("SCRATCHBIRD BENCHMARK COMPARISON REPORT")
        lines.append("=" * 70)
        lines.append("")

        # System Information (shared)
        if system_info:
            lines.append("SYSTEM INFORMATION")
            lines.append("-" * 70)
            cpu = system_info.get('cpu', {})
            lines.append(f"CPU: {cpu.get('model', 'Unknown')}")
            lines.append(f"Cores: {cpu.get('physical_cores', 'Unknown')} physical, {cpu.get('logical_cores', 'Unknown')} logical")
            memory = system_info.get('memory', {})
            lines.append(f"Memory: {memory.get('total_mb', 0) / 1024:.1f} GB")
            os_info = system_info.get('os', {})
            lines.append(f"OS: {os_info.get('distribution', os_info.get('name', 'Unknown'))}")
            lines.append("")

        # Comparison table
        lines.append("ENGINE COMPARISON")
        lines.append("-" * 70)

        # Extract data for comparison
        comparison_data = []
        for result in all_results:
            meta = result.get('metadata', {})
            summary = result.get('summary', {})
            comparison_data.append({
                'engine': meta.get('engine', 'Unknown'),
                'suite': meta.get('suite', 'Unknown'),
                'total': summary.get('total_tests', 0),
                'passed': summary.get('passed', 0),
                'failed': summary.get('failed', 0),
                'score': summary.get('score', 'N/A')
            })

        # Print comparison table
        lines.append(f"{'Engine':<15} {'Suite':<15} {'Total':<8} {'Passed':<8} {'Failed':<8} {'Score':<10}")
        lines.append("-" * 70)
        for data in comparison_data:
            lines.append(f"{data['engine']:<15} {data['suite']:<15} {data['total']:<8} {data['passed']:<8} {data['failed']:<8} {data['score']:<10}")
        lines.append("")

        # Detailed breakdown
        lines.append("DETAILED BREAKDOWN")
        lines.append("-" * 70)

        for result in all_results:
            meta = result.get('metadata', {})
            summary = result.get('summary', {})
            engine = meta.get('engine', 'Unknown')

            lines.append(f"")
            lines.append(f"{engine.upper()}")
            lines.append(f"  Total Tests: {summary.get('total_tests', 'N/A')}")
            lines.append(f"  Passed:      {summary.get('passed', 'N/A')}")
            lines.append(f"  Failed:      {summary.get('failed', 'N/A')}")
            lines.append(f"  Errors:      {summary.get('errors', 'N/A')}")
            lines.append(f"  Score:       {summary.get('score', 'N/A')}")

        lines.append("")
        lines.append("=" * 70)
        lines.append("END OF COMPARISON REPORT")
        lines.append("=" * 70)

        content = "\n".join(lines)

        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        filename = f"benchmark_comparison_{timestamp}.txt"
        filepath = result_files[0].parent / filename

        return FormattedReport(
            title="Benchmark Comparison Report",
            content=content,
            filepath=filepath
        )

    def save_report(self, report: FormattedReport, output_dir: Optional[Path] = None) -> Path:
        """
        Save a formatted report to disk.

        Args:
            report: The FormattedReport to save
            output_dir: Directory to save in (default: current directory)

        Returns:
            Path to saved file
        """
        if output_dir:
            output_dir = Path(output_dir)
            output_dir.mkdir(parents=True, exist_ok=True)
            filepath = output_dir / report.filepath.name
        else:
            filepath = report.filepath

        with open(filepath, 'w') as f:
            f.write(report.content)

        return filepath


def interactive_formatter():
    """Interactive report formatting."""
    import glob

    print("=" * 70)
    print("SCRATCHBIRD BENCHMARK REPORT FORMATTER")
    print("=" * 70)
    print()
    print("This tool creates human-readable text reports from benchmark results.")
    print("The reports can be posted to GitHub issues or discussions.")
    print()

    # Find result files
    result_files = list(Path(".").rglob("results/**/*.json"))
    result_files = [f for f in result_files if 'system-info' not in f.name]

    if not result_files:
        print("No benchmark result files found.")
        benchmark_path = input("Enter path to benchmark JSON: ").strip()
        result_files = [Path(benchmark_path)]
    else:
        print(f"Found {len(result_files)} result files:")
        for i, f in enumerate(result_files[:10], 1):
            print(f"  {i}. {f}")
        if len(result_files) > 10:
            print(f"  ... and {len(result_files) - 10} more")

        print()
        print("Options:")
        print("  1-N:  Select single file")
        print("  all:  Compare all files")
        print()
        choice = input("Selection: ").strip()

        if choice.lower() == 'all':
            selected_files = result_files
        else:
            try:
                idx = int(choice) - 1
                selected_files = [result_files[idx]]
            except:
                selected_files = [Path(choice)]

    # Check for system info
    sysinfo_files = list(Path(".").rglob("**/system-info.json"))
    sysinfo_file = None
    if sysinfo_files:
        print(f"\nFound system info: {sysinfo_files[0]}")
        use_sysinfo = input("Include system info? [Y/n]: ").strip().lower()
        if use_sysinfo in ('', 'y', 'yes'):
            sysinfo_file = sysinfo_files[0]

    # Tags and notes
    tags_input = input("\nEnter tags (comma-separated, optional): ").strip()
    tags = [t.strip() for t in tags_input.split(",") if t.strip()]

    notes = input("Enter notes (optional): ").strip() or None

    # Generate report
    print("\nGenerating report...")
    formatter = TextReportFormatter()

    if len(selected_files) == 1:
        report = formatter.format_single_result(
            benchmark_file=selected_files[0],
            system_info_file=sysinfo_file,
            tags=tags,
            notes=notes
        )
    else:
        report = formatter.format_comparison(
            result_files=selected_files,
            system_info_file=sysinfo_file
        )

    # Save report
    output_path = formatter.save_report(report)

    print(f"\n✓ Report saved to: {output_path}")
    print()
    print("Preview:")
    print("-" * 70)
    print(report.content[:1000])
    if len(report.content) > 1000:
        print("...")
    print("-" * 70)
    print()
    print("To submit this report:")
    print("1. Open the file: cat", output_path)
    print("2. Copy the contents")
    print("3. Paste into a GitHub issue or discussion")


def main():
    """CLI entry point."""
    import argparse

    parser = argparse.ArgumentParser(
        description='Format benchmark results as human-readable text',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Interactive mode
  %(prog)s

  # Format single result
  %(prog)s --benchmark results/acid-postgresql-20240308.json

  # Format with system info
  %(prog)s --benchmark results/test.json --system-info system-info.json

  # Compare multiple results
  %(prog)s --compare results/*.json

  # With tags and notes
  %(prog)s --benchmark results/test.json --tags production,aws --notes "Initial run"
        """
    )

    parser.add_argument('--benchmark', '-b', type=Path,
                       help='Path to benchmark result JSON file')
    parser.add_argument('--system-info', '-s', type=Path,
                       help='Path to system info JSON file')
    parser.add_argument('--compare', '-c', nargs='+', type=Path,
                       help='Compare multiple benchmark files')
    parser.add_argument('--tags', '-t', nargs='+',
                       help='Tags for the report')
    parser.add_argument('--notes', '-n',
                       help='Notes for the report')
    parser.add_argument('--output', '-o', type=Path,
                       help='Output directory for the report')
    parser.add_argument('--stdout', action='store_true',
                       help='Print report to stdout instead of saving')

    args = parser.parse_args()

    # Interactive mode if no args
    if not args.benchmark and not args.compare:
        interactive_formatter()
        return

    formatter = TextReportFormatter()

    # Compare mode
    if args.compare:
        report = formatter.format_comparison(
            result_files=args.compare,
            system_info_file=args.system_info
        )

    # Single file mode
    elif args.benchmark:
        if not args.benchmark.exists():
            print(f"Error: File not found: {args.benchmark}")
            return

        report = formatter.format_single_result(
            benchmark_file=args.benchmark,
            system_info_file=args.system_info,
            tags=args.tags,
            notes=args.notes
        )

    else:
        parser.print_help()
        return

    # Output
    if args.stdout:
        print(report.content)
    else:
        output_path = formatter.save_report(report, args.output)
        print(f"Report saved to: {output_path}")
        print()
        print("To submit this report:")
        print("1. View the file:", output_path)
        print("2. Copy the contents")
        print("3. Paste into a GitHub issue or discussion at:")
        print("   https://github.com/DaltonCalford/ScratchBird-Benchmarks/issues")


if __name__ == '__main__':
    main()
