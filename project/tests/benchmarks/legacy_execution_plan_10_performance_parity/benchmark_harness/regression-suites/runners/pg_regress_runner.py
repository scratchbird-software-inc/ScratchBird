#!/usr/bin/env python3
"""
PostgreSQL pg_regress Runner

Executes PostgreSQL Regression Tests (.sql files) against:
1. Original PostgreSQL server (baseline)
2. ScratchBird in PostgreSQL mode (emulation)

Uses psql client to execute tests for authentic behavior.
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


class PostgreSQLExecutor:
    """Executes SQL against PostgreSQL using psql client."""

    def __init__(self, host: str, port: int, database: str,
                 user: str, password: str, timeout: int = 60):
        self.host = host
        self.port = port
        self.database = database
        self.user = user
        self.password = password
        self.timeout = timeout
        self.psql_bin = (
            os.environ.get('SCRATCHBIRD_PG_PSQL_BIN')
            or os.environ.get('SCRATCHBIRD_PG_ISQL')
            or os.environ.get('PG_PSQL_BIN')
            or 'psql'
        )
        self.env = os.environ.copy()
        self.env['PGPASSWORD'] = password

    def execute_psql(self, sql_script: str) -> Tuple[int, str, str]:
        """Execute SQL using psql command-line client."""
        import tempfile

        # Create temporary SQL file
        with tempfile.NamedTemporaryFile(mode='w', suffix='.sql', delete=False) as f:
            f.write(sql_script)
            sql_file = f.name

        try:
            # Build psql command
            cmd = [
                self.psql_bin,
                f'--host={self.host}',
                f'--port={self.port}',
                f'--username={self.user}',
                f'--dbname={self.database}',
                '--no-psqlrc',      # Ignore .psqlrc
                '--no-align',       # Unaligned output
                '--tuples-only',    # No header/footer
                '--set', 'ON_ERROR_STOP=1',
                '-f', sql_file
            ]

            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=self.timeout,
                env=self.env
            )

            return result.returncode, result.stdout, result.stderr
        except subprocess.TimeoutExpired:
            return -1, "", "Timeout"
        except FileNotFoundError:
            return self._execute_python(sql_script)
        finally:
            os.unlink(sql_file)

    def _execute_python(self, sql_script: str) -> Tuple[int, str, str]:
        """Fallback execution using Python PostgreSQL driver."""
        try:
            import psycopg2
            from psycopg2.extras import RealDictCursor

            conn = psycopg2.connect(
                host=self.host,
                port=self.port,
                dbname=self.database,
                user=self.user,
                password=self.password
            )
            conn.set_session(autocommit=False)

            stdout_lines = []
            stderr_lines = []

            try:
                # Split script into statements
                statements = self._split_statements(sql_script)

                for stmt in statements:
                    stmt = stmt.strip()
                    if not stmt:
                        continue

                    # Handle psql meta-commands
                    if stmt.startswith('\\'):
                        # Skip meta-commands for now
                        continue

                    try:
                        with conn.cursor() as cur:
                            cur.execute(stmt)

                            # Fetch results if any
                            if cur.description:
                                rows = cur.fetchall()
                                for row in rows:
                                    # Format like psql unaligned output
                                    row_str = '\t'.join(str(col) if col is not None else '' for col in row)
                                    stdout_lines.append(row_str)

                            conn.commit()
                    except psycopg2.Error as e:
                        stderr_lines.append(f"ERROR: {e.pgerror or str(e)}")
                        conn.rollback()

                return 0, '\n'.join(stdout_lines), '\n'.join(stderr_lines)
            finally:
                conn.close()

        except ImportError:
            return -1, "", "psycopg2 not available and psql client not found"
        except Exception as e:
            return -1, "", str(e)

    @staticmethod
    def _split_statements(script: str) -> List[str]:
        """Split SQL script into individual statements."""
        statements = []
        current = []
        in_string = False
        string_char = None

        for line in script.split('\n'):
            # Check for psql meta-commands
            if line.strip().startswith('\\'):
                if current:
                    statements.append('\n'.join(current))
                    current = []
                statements.append(line.strip())
                continue

            # Handle string literals
            i = 0
            while i < len(line):
                char = line[i]

                if not in_string:
                    if char in ("'", '"'):
                        in_string = True
                        string_char = char
                else:
                    if char == string_char:
                        # Check for escaped quote
                        if i + 1 < len(line) and line[i + 1] == string_char:
                            i += 1
                        else:
                            in_string = False
                            string_char = None

                i += 1

            current.append(line)

            # End of statement
            if not in_string and line.rstrip().endswith(';'):
                statements.append('\n'.join(current))
                current = []

        if current:
            statements.append('\n'.join(current))

        return statements


class PGRegressRunner:
    """Main PostgreSQL regression test runner."""

    def __init__(self, pg_src_path: Path, output_dir: Path,
                 target: str = "original", mode: str = "postgresql"):
        self.pg_src_path = pg_src_path
        self.output_dir = output_dir
        self.target = target
        self.mode = mode
        self.results: List[TestResult] = []

        # Configuration
        if target == "original":
            self.pg_config = {
                'host': 'postgresql',
                'port': 5432,
                'database': 'benchmark',
                'user': 'benchmark',
                'password': 'benchmark'
            }
        else:  # scratchbird
            self.pg_config = {
                'host': 'scratchbird',
                'port': 5432,  # PostgreSQL protocol port
                'database': 'benchmark',
                'user': 'benchmark',
                'password': 'benchmark'
            }

        self.executor = PostgreSQLExecutor(**self.pg_config)

    def discover_tests(self, schedule: str = "parallel") -> List[Tuple[Path, Path]]:
        """Discover test files and their expected results from schedule."""
        regress_dir = self.pg_src_path / "src" / "test" / "regress"
        sql_dir = regress_dir / "sql"
        expected_dir = regress_dir / "expected"

        test_files = []

        if schedule == "parallel":
            # Read parallel schedule
            schedule_file = regress_dir / "parallel_schedule"
            if schedule_file.exists():
                test_names = self._parse_schedule(schedule_file)
            else:
                # Fallback: discover all SQL files
                test_names = [f.stem for f in sorted(sql_dir.glob("*.sql"))]
        elif schedule == "serial":
            schedule_file = regress_dir / "serial_schedule"
            if schedule_file.exists():
                test_names = self._parse_schedule(schedule_file)
            else:
                test_names = []
        else:
            # Specific test name
            test_names = [schedule]

        for test_name in test_names:
            sql_file = sql_dir / f"{test_name}.sql"
            expected_file = expected_dir / f"{test_name}.out"

            if sql_file.exists() and expected_file.exists():
                test_files.append((sql_file, expected_file))

        return test_files

    def _parse_schedule(self, schedule_file: Path) -> List[str]:
        """Parse a PostgreSQL test schedule file."""
        tests = []
        content = schedule_file.read_text()

        for line in content.split('\n'):
            line = line.strip()

            # Skip comments and empty lines
            if not line or line.startswith('#'):
                continue

            # Skip schedule directives
            if line.startswith('test:') or line.startswith('ignore:'):
                # Extract test names
                test_part = line.split(':', 1)[1].strip()
                tests.extend(test_part.split())

        return tests

    def run_test(self, sql_path: Path, expected_path: Path) -> TestResult:
        """Run a single PostgreSQL regression test."""
        start_time = time.time()

        test_name = sql_path.stem

        # Load SQL and expected output
        sql_script = sql_path.read_text(encoding='utf-8', errors='ignore')
        expected_output = expected_path.read_text(encoding='utf-8', errors='ignore')

        # Execute
        returncode, stdout, stderr = self.executor.execute_psql(sql_script)

        duration_ms = (time.time() - start_time) * 1000

        # Build actual output
        actual_output = stdout
        if stderr:
            actual_output += "\n" + stderr

        # Normalize for comparison
        normalized_actual = self._normalize_output(actual_output)
        normalized_expected = self._normalize_output(expected_output)

        # Determine result
        if returncode != 0:
            status = "FAIL"
            error_message = stderr[:500] if stderr else f"Exit code: {returncode}"
        elif normalized_actual == normalized_expected:
            status = "PASS"
        else:
            # Check for known differences (platform-specific, etc.)
            if self._check_equivalent(normalized_actual, normalized_expected):
                status = "PASS_EQUIVALENT"
            else:
                status = "FAIL"
                error_message = "Output mismatch"

        return TestResult(
            test_id=test_name,
            status=status,
            duration_ms=duration_ms,
            expected_output=expected_output[:1000],
            actual_output=actual_output[:1000],
            diff=self._generate_diff(expected_output, actual_output) if status in ("FAIL",) else "",
            error_message=error_message if 'error_message' in dir() else ""
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

    def _check_equivalent(self, actual: str, expected: str) -> bool:
        """Check if two outputs are semantically equivalent."""
        # Normalize whitespace
        actual_clean = '\n'.join(' '.join(line.split()) for line in actual.split('\n') if line.strip())
        expected_clean = '\n'.join(' '.join(line.split()) for line in expected.split('\n') if line.strip())
        return actual_clean == expected_clean

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

    def run_suite(self, schedule: str = "parallel", limit: Optional[int] = None,
                  exclusions: Optional[Set[str]] = None):
        """Run a complete test suite."""
        test_files = self.discover_tests(schedule)
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

        for i, (sql_path, expected_path) in enumerate(test_files, 1):
            test_name = sql_path.stem

            # Check exclusions
            if test_name in exclusions:
                print(f"[{i}/{len(test_files)}] SKIP (excluded): {test_name}")
                skipped += 1
                continue

            print(f"[{i}/{len(test_files)}] Running: {test_name}...", end=' ', flush=True)

            try:
                result = self.run_test(sql_path, expected_path)
                self.results.append(result)

                print(result.status)

                if result.status in ("PASS", "PASS_EQUIVALENT"):
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
        results_file = self.output_dir / f"pg_regress_results_{self.target}_{timestamp}.json"

        results_data = {
            'metadata': {
                'target': self.target,
                'mode': self.mode,
                'timestamp': timestamp,
                'pg_src_path': str(self.pg_src_path),
            },
            'summary': {
                'total': len(self.results),
                'passed': sum(1 for r in self.results if r.status in ('PASS', 'PASS_EQUIVALENT')),
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
    parser = argparse.ArgumentParser(description='PostgreSQL Regression Test Runner')
    parser.add_argument('--pg-src-path', type=Path,
                        default=Path('/postgresql'),
                        help='Path to PostgreSQL source')
    parser.add_argument('--schedule', default='parallel',
                        help='Test schedule (parallel, serial, or specific test name)')
    parser.add_argument('--target', default='original',
                        choices=['original', 'scratchbird'],
                        help='Target engine to test')
    parser.add_argument('--mode', default='postgresql',
                        choices=['postgresql', 'mysql', 'firebird'],
                        help='ScratchBird emulation mode (if target=scratchbird)')
    parser.add_argument('--output-dir', type=Path, default=Path('/results'),
                        help='Output directory for results')
    parser.add_argument('--limit', type=int, default=None,
                        help='Limit number of tests to run')

    args = parser.parse_args()

    # Ensure output directory exists
    args.output_dir.mkdir(parents=True, exist_ok=True)

    # Create and run
    runner = PGRegressRunner(
        pg_src_path=args.pg_src_path,
        output_dir=args.output_dir,
        target=args.target,
        mode=args.mode
    )

    runner.run_suite(
        schedule=args.schedule,
        limit=args.limit
    )


if __name__ == '__main__':
    main()
