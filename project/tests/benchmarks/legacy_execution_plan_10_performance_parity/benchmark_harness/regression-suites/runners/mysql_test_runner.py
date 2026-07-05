#!/usr/bin/env python3
"""
MySQL mysql-test Runner

Executes MySQL Test Suite (.test files) against:
1. Original MySQL server (baseline)
2. ScratchBird in MySQL mode (emulation)

Uses mysql client to execute tests to ensure authentic behavior.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional, Tuple, Set


@dataclass
class TestResult:
    """Result of a single test execution."""
    test_id: str
    status: str  # PASS, FAIL, SKIP, ERROR
    duration_ms: float
    expected_output: str = ""
    actual_output: str = ""
    diff: str = ""
    error_message: str = ""
    skipped_reason: str = ""


class MySQLExecutor:
    """Executes SQL against MySQL using mysql client."""

    def __init__(self, host: str, port: int, database: str,
                 user: str, password: str, timeout: int = 60):
        self.host = host
        self.port = port
        self.database = database
        self.user = user
        self.password = password
        self.timeout = timeout
        self.mysql_bin = (
            os.environ.get('SCRATCHBIRD_MYSQL_CLI_BIN')
            or os.environ.get('SCRATCHBIRD_MY_ISQL')
            or os.environ.get('MYSQL_CLI_BIN')
            or 'mysql'
        )

    def execute_mysql(self, sql_script: str) -> Tuple[int, str, str]:
        """Execute SQL using mysql command-line client."""
        import tempfile

        # Create temporary SQL file
        with tempfile.NamedTemporaryFile(mode='w', suffix='.sql', delete=False) as f:
            f.write(sql_script)
            sql_file = f.name

        try:
            # Build mysql command
            cmd = [
                self.mysql_bin,
                f'--host={self.host}',
                f'--port={self.port}',
                f'--user={self.user}',
                f'--password={self.password}',
                '--batch',  # Tab-separated output
                '--raw',    # No escaping
                '--skip-column-names',  # We'll handle headers separately
                self.database,
                '-e', f'source {sql_file}'
            ]

            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=self.timeout
            )

            return result.returncode, result.stdout, result.stderr
        except subprocess.TimeoutExpired:
            return -1, "", "Timeout"
        except FileNotFoundError:
            return self._execute_python(sql_script)
        finally:
            os.unlink(sql_file)

    def _execute_python(self, sql_script: str) -> Tuple[int, str, str]:
        """Fallback execution using Python MySQL driver."""
        try:
            import pymysql

            conn = pymysql.connect(
                host=self.host,
                port=self.port,
                database=self.database,
                user=self.user,
                password=self.password,
                charset='utf8mb4'
            )

            stdout_lines = []
            stderr_lines = []

            try:
                # Split script into statements
                statements = self._split_statements(sql_script)

                for stmt in statements:
                    stmt = stmt.strip()
                    if not stmt or stmt.startswith('--') or stmt.startswith('#'):
                        continue

                    # Handle mysql-test special commands
                    if stmt.startswith('--echo'):
                        stdout_lines.append(stmt[6:].strip())
                        continue
                    if stmt.startswith('--error'):
                        # Expected error, handled in comparison
                        continue

                    try:
                        with conn.cursor() as cur:
                            cur.execute(stmt)

                            # Fetch results if any
                            if cur.description:
                                # Format like mysql client
                                headers = [desc[0] for desc in cur.description]

                                rows = cur.fetchall()
                                for row in rows:
                                    row_str = '\t'.join(str(col) for col in row)
                                    stdout_lines.append(row_str)

                            conn.commit()
                    except pymysql.Error as e:
                        stderr_lines.append(f"ERROR {e.args[0]}: {e.args[1]}")
                        conn.rollback()

                return 0, '\n'.join(stdout_lines), '\n'.join(stderr_lines)
            finally:
                conn.close()

        except ImportError:
            return -1, "", "pymysql not available and mysql client not found"
        except Exception as e:
            return -1, "", str(e)

    @staticmethod
    def _split_statements(script: str) -> List[str]:
        """Split SQL script into individual statements."""
        statements = []
        current = []
        delimiter = ';'
        in_delimiter_cmd = False

        for line in script.split('\n'):
            line_stripped = line.strip()

            # Handle delimiter command
            if line_stripped.upper().startswith('DELIMITER '):
                delimiter = line_stripped.split()[1]
                in_delimiter_cmd = True
                continue

            # Skip comments and special mysql-test commands for splitting
            if line_stripped.startswith('--') or line_stripped.startswith('#'):
                statements.append(line)
                continue

            current.append(line)

            # End of statement
            if line_stripped.endswith(delimiter):
                stmt = '\n'.join(current)
                if delimiter != ';':
                    stmt = stmt.replace(delimiter, ';')
                statements.append(stmt)
                current = []
                if in_delimiter_cmd:
                    delimiter = ';'
                    in_delimiter_cmd = False

        if current:
            statements.append('\n'.join(current))

        return statements


class MySQLTestRunner:
    """Main MySQL test runner."""

    def __init__(self, mysql_test_path: Path, output_dir: Path,
                 target: str = "original", mode: str = "mysql"):
        self.mysql_test_path = mysql_test_path
        self.output_dir = output_dir
        self.target = target
        self.mode = mode
        self.results: List[TestResult] = []

        # Configuration
        if target == "original":
            self.mysql_config = {
                'host': 'mysql',
                'port': 3306,
                'database': 'benchmark',
                'user': 'benchmark',
                'password': 'benchmark'
            }
        else:  # scratchbird
            self.mysql_config = {
                'host': 'scratchbird',
                'port': 3306,  # MySQL protocol port
                'database': 'benchmark',
                'user': 'benchmark',
                'password': 'benchmark'
            }

        self.executor = MySQLExecutor(**self.mysql_config)

    def discover_tests(self, suite: str = "all") -> List[Tuple[Path, Path]]:
        """Discover test files and their expected results."""
        test_dir = self.mysql_test_path / "t"
        result_dir = self.mysql_test_path / "r"

        test_files = []

        if suite == "all":
            # Get all .test files in t/ directory
            for test_file in sorted(test_dir.glob("*.test")):
                result_file = result_dir / f"{test_file.stem}.result"
                if result_file.exists():
                    test_files.append((test_file, result_file))
        else:
            # Suite-specific tests
            suite_test_dir = self.mysql_test_path / "suite" / suite / "t"
            suite_result_dir = self.mysql_test_path / "suite" / suite / "r"

            if suite_test_dir.exists():
                for test_file in sorted(suite_test_dir.glob("*.test")):
                    result_file = suite_result_dir / f"{test_file.stem}.result"
                    if result_file.exists():
                        test_files.append((test_file, result_file))

        return test_files

    def parse_test_file(self, test_path: Path) -> Tuple[str, List[str], List[int]]:
        """Parse a .test file extracting SQL and metadata.

        Returns:
            (sql_script, echo_commands, expected_errors)
        """
        content = test_path.read_text(encoding='utf-8', errors='ignore')

        sql_lines = []
        echo_commands = []
        expected_errors = []

        for line in content.split('\n'):
            line_stripped = line.strip()

            # Handle mysql-test commands
            if line_stripped.startswith('--echo'):
                echo_commands.append(line_stripped[6:].strip())
            elif line_stripped.startswith('--error'):
                # Parse expected error codes
                error_part = line_stripped[7:].strip()
                codes = [int(x) for x in error_part.split(',') if x.strip().isdigit()]
                expected_errors.extend(codes)
            elif line_stripped.startswith('--'):
                # Other mysql-test directives (skip for now)
                continue
            elif line_stripped.startswith('#'):
                # Comment
                continue
            else:
                sql_lines.append(line)

        return '\n'.join(sql_lines), echo_commands, expected_errors

    def run_test(self, test_path: Path, result_path: Path) -> TestResult:
        """Run a single MySQL test."""
        start_time = time.time()

        test_name = test_path.stem

        # Parse test file
        sql_script, echo_commands, expected_errors = self.parse_test_file(test_path)

        # Load expected output
        expected_output = result_path.read_text(encoding='utf-8', errors='ignore')

        # Execute
        returncode, stdout, stderr = self.executor.execute_mysql(sql_script)

        duration_ms = (time.time() - start_time) * 1000

        # Build actual output
        actual_output = stdout
        if stderr:
            actual_output += "\n" + stderr

        # Normalize for comparison
        normalized_actual = self._normalize_output(actual_output)
        normalized_expected = self._normalize_output(expected_output)

        # Check for expected errors
        if expected_errors:
            # If we expected an error, check that we got one
            error_found = any(f"ERROR {code}" in stderr for code in expected_errors)
            if error_found or returncode != 0:
                status = "PASS"
            else:
                status = "FAIL"
                error_message = "Expected error but got success"
        elif returncode != 0:
            status = "FAIL"
            error_message = stderr[:500]
        elif normalized_actual == normalized_expected:
            status = "PASS"
        else:
            status = "FAIL"
            error_message = "Output mismatch"

        return TestResult(
            test_id=test_name,
            status=status,
            duration_ms=duration_ms,
            expected_output=expected_output[:1000],
            actual_output=actual_output[:1000],
            diff=self._generate_diff(expected_output, actual_output) if status == "FAIL" else "",
            error_message=error_message if status == "FAIL" else ""
        )

    def _normalize_output(self, output: str) -> str:
        """Normalize output for comparison."""
        lines = output.strip().split('\n')
        normalized = []
        for line in lines:
            # Remove trailing whitespace
            line = line.rstrip()
            # Skip empty lines
            if line:
                normalized.append(line)
        return '\n'.join(normalized)

    def _generate_diff(self, expected: str, actual: str) -> str:
        """Generate unified diff between expected and actual."""
        import difflib
        expected_lines = expected.splitlines(keepends=True)
        actual_lines = actual.splitlines(keepends=True)

        diff = difflib.unified_diff(
            expected_lines,
            actual_lines,
            fromfile='expected',
            tofile='actual',
            lineterm=''
        )
        return '\n'.join(list(diff)[:50])  # Limit diff size

    def run_suite(self, suite: str = "all", limit: Optional[int] = None,
                  exclusions: Optional[Set[str]] = None):
        """Run a complete test suite."""
        test_files = self.discover_tests(suite)
        print(f"Discovered {len(test_files)} test files")

        if limit:
            test_files = test_files[:limit]
            print(f"Limited to {limit} tests")

        exclusions = exclusions or set()
        print(f"Loaded {len(exclusions)} exclusions")

        passed = 0
        failed = 0
        skipped = 0
        errors = 0

        for i, (test_path, result_path) in enumerate(test_files, 1):
            test_name = test_path.stem

            # Check exclusions
            if test_name in exclusions:
                print(f"[{i}/{len(test_files)}] SKIP (excluded): {test_name}")
                skipped += 1
                continue

            print(f"[{i}/{len(test_files)}] Running: {test_name}...", end=' ', flush=True)

            try:
                result = self.run_test(test_path, result_path)
                self.results.append(result)

                print(result.status)

                if result.status == "PASS":
                    passed += 1
                elif result.status == "SKIP":
                    skipped += 1
                elif result.status == "ERROR":
                    errors += 1
                else:
                    failed += 1
            except Exception as e:
                print(f"ERROR: {e}")
                errors += 1
                self.results.append(TestResult(
                    test_id=test_name,
                    status="ERROR",
                    duration_ms=0,
                    error_message=str(e)
                ))

        print(f"\n{'='*60}")
        print(f"Results: {passed} passed, {failed} failed, {skipped} skipped, {errors} errors")

        self._save_results()

    def _save_results(self):
        """Save results to JSON file."""
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        results_file = self.output_dir / f"mysql_test_results_{self.target}_{timestamp}.json"

        results_data = {
            'metadata': {
                'target': self.target,
                'mode': self.mode,
                'timestamp': timestamp,
                'mysql_test_path': str(self.mysql_test_path),
            },
            'summary': {
                'total': len(self.results),
                'passed': sum(1 for r in self.results if r.status == 'PASS'),
                'failed': sum(1 for r in self.results if r.status == 'FAIL'),
                'skipped': sum(1 for r in self.results if r.status == 'SKIP'),
                'errors': sum(1 for r in self.results if r.status == 'ERROR'),
            },
            'results': [
                {
                    'test_id': r.test_id,
                    'status': r.status,
                    'duration_ms': r.duration_ms,
                    'error_message': r.error_message,
                }
                for r in self.results
            ]
        }

        results_file.write_text(json.dumps(results_data, indent=2))
        print(f"\nResults saved to: {results_file}")


def main():
    parser = argparse.ArgumentParser(description='MySQL Test Runner')
    parser.add_argument('--mysql-test-path', type=Path,
                        default=Path('/mysql-server/mysql-test'),
                        help='Path to MySQL test suite')
    parser.add_argument('--suite', default='all',
                        help='Test suite to run (all, funcs_1, sys_vars, etc.)')
    parser.add_argument('--target', default='original',
                        choices=['original', 'scratchbird'],
                        help='Target engine to test')
    parser.add_argument('--mode', default='mysql',
                        choices=['mysql', 'firebird', 'postgresql'],
                        help='ScratchBird emulation mode (if target=scratchbird)')
    parser.add_argument('--output-dir', type=Path, default=Path('/results'),
                        help='Output directory for results')
    parser.add_argument('--limit', type=int, default=None,
                        help='Limit number of tests to run')

    args = parser.parse_args()

    # Ensure output directory exists
    args.output_dir.mkdir(parents=True, exist_ok=True)

    # Create and run
    runner = MySQLTestRunner(
        mysql_test_path=args.mysql_test_path,
        output_dir=args.output_dir,
        target=args.target,
        mode=args.mode
    )

    runner.run_suite(
        suite=args.suite,
        limit=args.limit
    )


if __name__ == '__main__':
    main()
