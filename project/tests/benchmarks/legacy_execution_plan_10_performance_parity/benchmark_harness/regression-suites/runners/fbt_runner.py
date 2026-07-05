#!/usr/bin/env python3
"""
Firebird FBT Test Runner

Executes Firebird Test Suite (.fbt files) against:
1. Original FirebirdSQL server (baseline)
2. ScratchBird in Firebird mode (emulation)

Usage:
    python fbt_runner.py --suite=bugs --target=original
    python fbt_runner.py --suite=bugs --target=scratchbird --mode=firebird
"""

import argparse
import ast
import json
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional, Tuple


@dataclass
class TestResult:
    """Result of a single test execution."""
    test_id: str
    tracker_id: str
    title: str
    status: str  # PASS, FAIL, SKIP, ERROR
    duration_ms: float
    expected_output: str = ""
    actual_output: str = ""
    diff: str = ""
    error_message: str = ""
    skipped_reason: str = ""


@dataclass
class FBTTest:
    """Parsed FBT test file."""
    test_id: str
    tracker_id: str
    title: str
    description: str
    min_versions: str
    versions: List[Dict]
    raw_content: str


class FBTParser:
    """Parser for .fbt test files."""

    @staticmethod
    def parse(fbt_path: Path) -> Optional[FBTTest]:
        """Parse a .fbt file into a test object."""
        try:
            content = fbt_path.read_text(encoding='utf-8', errors='ignore')

            # FBT files use Python-like dict syntax (single quotes)
            # Convert to valid JSON for parsing
            try:
                # Try parsing as Python literal
                data = ast.literal_eval(content)
            except (SyntaxError, ValueError) as e:
                # Try cleaning up the content
                cleaned = FBTParser._clean_fbt_content(content)
                try:
                    data = ast.literal_eval(cleaned)
                except:
                    return None

            versions = data.get('versions', [])
            if not versions:
                return None

            return FBTTest(
                test_id=data.get('id', ''),
                tracker_id=data.get('tracker_id', ''),
                title=data.get('title', ''),
                description=data.get('description', ''),
                min_versions=data.get('min_versions', ''),
                versions=versions,
                raw_content=content
            )
        except Exception as e:
            print(f"Error parsing {fbt_path}: {e}", file=sys.stderr)
            return None

    @staticmethod
    def _clean_fbt_content(content: str) -> str:
        """Clean FBT content to make it parseable."""
        # Replace single quotes with double quotes for JSON compatibility
        # But be careful with quoted strings inside
        lines = content.split('\n')
        cleaned_lines = []
        for line in lines:
            # Remove trailing comments
            if '#' in line and not line.strip().startswith('#'):
                line = line.split('#')[0]
            cleaned_lines.append(line)
        return '\n'.join(cleaned_lines)


class FirebirdExecutor:
    """Executes SQL against Firebird using isql."""

    def __init__(self, host: str, port: int, database: str,
                 user: str, password: str, timeout: int = 120):
        self.host = host
        self.port = port
        self.database = database
        self.user = user
        self.password = password
        self.timeout = timeout
        self.isql_bin = (
            os.environ.get('SCRATCHBIRD_FB_NATIVE_ISQL')
            or os.environ.get('SCRATCHBIRD_FB_ISQL')
            or os.environ.get('FIREBIRD_ISQL_BIN')
            or 'isql'
        )
        self.env = os.environ.copy()
        isql_path = Path(self.isql_bin)
        if isql_path.is_absolute():
            firebird_root = isql_path.parent.parent
            firebird_lib = firebird_root / 'lib'
            self.env.setdefault('FIREBIRD', str(firebird_root))
            if firebird_lib.exists():
                existing = self.env.get('LD_LIBRARY_PATH', '')
                self.env['LD_LIBRARY_PATH'] = (
                    f"{firebird_lib}:{existing}" if existing else str(firebird_lib)
                )

    def execute_isql(self, sql_script: str) -> Tuple[int, str, str]:
        """Execute SQL using isql command-line tool."""
        # Build connection string
        dsn = f"{self.host}/{self.port}:{self.database}"

        # Create temporary SQL file
        import tempfile
        with tempfile.NamedTemporaryFile(mode='w', suffix='.sql', delete=False) as f:
            f.write(sql_script)
            sql_file = f.name

        try:
            # Build isql command
            cmd = [
                self.isql_bin,
                '-u', self.user,
                '-p', self.password,
                '-i', sql_file,
                dsn
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
            # isql not found, try using Python driver
            return self._execute_python(sql_script)
        finally:
            os.unlink(sql_file)

    def _execute_python(self, sql_script: str) -> Tuple[int, str, str]:
        """Fallback execution using Python Firebird driver."""
        try:
            import fdb

            conn = fdb.connect(
                host=self.host,
                port=self.port,
                database=self.database,
                user=self.user,
                password=self.password
            )

            stdout_lines = []
            stderr_lines = []

            try:
                # Split script into statements
                statements = self._split_statements(sql_script)

                for stmt in statements:
                    stmt = stmt.strip()
                    if not stmt:
                        continue

                    try:
                        cur = conn.cursor()
                        cur.execute(stmt)

                        # Fetch results if any
                        if cur.description:
                            # Get column names
                            headers = [desc[0] for desc in cur.description]
                            stdout_lines.append(' '.join(headers))
                            stdout_lines.append(' '.join(['=' * len(h) for h in headers]))

                            rows = cur.fetchall()
                            for row in rows:
                                row_str = ' '.join(str(col) for col in row)
                                stdout_lines.append(row_str)
                        else:
                            stdout_lines.append(f"Rows affected: {cur.rowcount}")

                        conn.commit()
                    except Exception as e:
                        stderr_lines.append(str(e))
                        conn.rollback()

                return 0, '\n'.join(stdout_lines), '\n'.join(stderr_lines)
            finally:
                conn.close()

        except ImportError:
            return -1, "", "fdb module not available and isql not found"
        except Exception as e:
            return -1, "", str(e)

    @staticmethod
    def _split_statements(script: str) -> List[str]:
        """Split SQL script into individual statements."""
        # Simple split on semicolons, handling PSQL blocks
        statements = []
        current = []
        in_psql = False

        for line in script.split('\n'):
            line_stripped = line.strip().upper()

            # Detect PSQL block start
            if any(kw in line_stripped for kw in ['CREATE PROCEDURE', 'CREATE TRIGGER',
                                                     'CREATE FUNCTION', 'ALTER PROCEDURE']):
                in_psql = True

            # Detect SET TERM - custom delimiter handling
            if line_stripped.startswith('SET TERM'):
                continue

            current.append(line)

            # End of statement
            if ';' in line and not in_psql:
                statements.append('\n'.join(current))
                current = []

            # End of PSQL block
            if in_psql and line_stripped == 'END' or line_stripped.startswith('END^'):
                in_psql = False
                statements.append('\n'.join(current))
                current = []

        if current:
            statements.append('\n'.join(current))

        return statements


class FBTRunner:
    """Main FBT test runner."""

    def __init__(self, fbt_path: Path, output_dir: Path,
                 target: str = "original", mode: str = "firebird"):
        self.fbt_path = fbt_path
        self.output_dir = output_dir
        self.target = target
        self.mode = mode
        self.results: List[TestResult] = []

        # Configuration
        if target == "original":
            self.firebird_config = {
                'host': 'firebird',
                'port': 3050,
                'database': 'benchmark.fdb',
                'user': 'benchmark',
                'password': 'benchmark'
            }
        else:  # scratchbird
            self.firebird_config = {
                'host': 'scratchbird',
                'port': 3050,  # Firebird wire protocol port
                'database': 'benchmark',
                'user': 'benchmark',
                'password': 'benchmark'
            }

        self.executor = FirebirdExecutor(**self.firebird_config)

    def discover_tests(self, suite: str = "all") -> List[Path]:
        """Discover test files in the FBT repository."""
        tests_dir = self.fbt_path / "tests"
        test_files = []

        if suite == "all" or suite == "bugs":
            bugs_dir = tests_dir / "bugs"
            if bugs_dir.exists():
                test_files.extend(sorted(bugs_dir.glob("*.fbt")))

        if suite == "all" or suite.startswith("functional"):
            functional_dir = tests_dir / "functional"
            if functional_dir.exists():
                if suite == "all":
                    test_files.extend(sorted(functional_dir.rglob("*.fbt")))
                else:
                    # Specific functional subdirectory
                    subdir = suite.split('.')[-1] if '.' in suite else suite
                    specific_dir = functional_dir / subdir
                    if specific_dir.exists():
                        test_files.extend(sorted(specific_dir.glob("*.fbt")))

        return test_files

    def load_exclusions(self, version: str = "FB50") -> set:
        """Load exclusion list for a specific Firebird version."""
        exclusions = set()
        exclude_file = self.fbt_path / "tests" / f"{version}-exclude-list.txt"

        if exclude_file.exists():
            for line in exclude_file.read_text().split('\n'):
                line = line.strip()
                if line and not line.startswith('#'):
                    exclusions.add(line)

        return exclusions

    def run_test(self, test_path: Path) -> TestResult:
        """Run a single FBT test."""
        start_time = time.time()

        # Parse test
        test = FBTParser.parse(test_path)
        if not test:
            return TestResult(
                test_id=str(test_path),
                tracker_id="",
                title="Parse Error",
                status="ERROR",
                duration_ms=0,
                error_message="Failed to parse test file"
            )

        # Find appropriate version
        version_info = None
        for v in test.versions:
            # Check if this version is compatible
            fb_ver = v.get('firebird_version', '')
            # Accept 5.0 tests
            if '5.0' in fb_ver or '3.0' in fb_ver or '4.0' in fb_ver or '2.5' in fb_ver:
                version_info = v
                break

        if not version_info:
            return TestResult(
                test_id=test.test_id,
                tracker_id=test.tracker_id,
                title=test.title,
                status="SKIP",
                duration_ms=0,
                skipped_reason="No compatible Firebird version found"
            )

        # Check test type
        test_type = version_info.get('test_type', 'ISQL')
        if test_type != 'ISQL':
            return TestResult(
                test_id=test.test_id,
                tracker_id=test.tracker_id,
                title=test.title,
                status="SKIP",
                duration_ms=0,
                skipped_reason=f"Test type '{test_type}' not yet supported"
            )

        # Build complete script
        init_script = version_info.get('init_script', '')
        test_script = version_info.get('test_script', '')
        cleanup_script = version_info.get('cleanup_script', '')

        full_script = f"""
{init_script}

{test_script}

{cleanup_script}
"""

        # Execute
        returncode, stdout, stderr = self.executor.execute_isql(full_script)

        duration_ms = (time.time() - start_time) * 1000

        # Get expected output
        expected_stdout = version_info.get('expected_stdout', '')
        expected_stderr = version_info.get('expected_stderr', '')
        substitutions = version_info.get('substitutions', [])

        filtered_stdout = self._apply_substitutions(stdout, substitutions)
        filtered_stderr = self._apply_substitutions(stderr, substitutions)
        filtered_combined = filtered_stdout + filtered_stderr

        # Normalize outputs for comparison
        normalized_actual = self._normalize_output(filtered_combined)
        normalized_expected = self._normalize_output(expected_stdout + expected_stderr)

        # Determine result
        if returncode != 0 and not expected_stderr:
            status = "FAIL"
            error_message = stderr or "Non-zero exit code"
        elif normalized_actual == normalized_expected:
            status = "PASS"
        else:
            # Check if semantically equivalent
            if self._check_equivalent(normalized_actual, normalized_expected):
                status = "PASS_EQUIVALENT"
            else:
                status = "FAIL"

        return TestResult(
            test_id=test.test_id,
            tracker_id=test.tracker_id,
            title=test.title,
            status=status,
            duration_ms=duration_ms,
            expected_output=expected_stdout,
            actual_output=filtered_combined,
            diff=self._generate_diff(expected_stdout + expected_stderr, filtered_combined)
                if status == "FAIL" else "",
            error_message=filtered_stderr if returncode != 0 else ""
        )

    def _apply_substitutions(self, output: str, substitutions) -> str:
        """Apply reference-defined regex substitutions to runtime output."""
        rewritten = output
        for entry in substitutions or []:
            if not isinstance(entry, (list, tuple)) or len(entry) != 2:
                continue
            pattern, replacement = entry
            try:
                rewritten = re.sub(pattern, replacement, rewritten, flags=re.MULTILINE)
            except re.error:
                continue
        return rewritten

    def _normalize_output(self, output: str) -> str:
        """Normalize output for comparison."""
        lines = output.strip().split('\n')
        normalized = []
        for line in lines:
            # Remove leading/trailing whitespace
            line = line.strip()
            if re.match(r"^After line \d+ in file .+$", line):
                continue
            # Skip empty lines
            if line:
                normalized.append(line)
        return '\n'.join(normalized)

    def _check_equivalent(self, actual: str, expected: str) -> bool:
        """Check if two outputs are semantically equivalent."""
        # TODO: Implement more sophisticated equivalence checking
        # For now, do simple whitespace-insensitive comparison
        actual_clean = ' '.join(actual.split())
        expected_clean = ' '.join(expected.split())
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
            tofile='actual'
        )
        return ''.join(diff)

    def run_suite(self, suite: str = "all", limit: Optional[int] = None,
                  exclusions: Optional[set] = None):
        """Run a complete test suite."""
        test_files = self.discover_tests(suite)
        print(f"Discovered {len(test_files)} test files")

        if limit:
            test_files = test_files[:limit]
            print(f"Limited to {limit} tests")

        # Load exclusions
        if exclusions is None:
            exclusions = self.load_exclusions()
        print(f"Loaded {len(exclusions)} exclusions")

        passed = 0
        failed = 0
        skipped = 0
        errors = 0

        for i, test_path in enumerate(test_files, 1):
            test_name = test_path.stem

            # Check exclusions
            if test_name in exclusions or str(test_path.relative_to(self.fbt_path)) in exclusions:
                print(f"[{i}/{len(test_files)}] SKIP (excluded): {test_name}")
                skipped += 1
                continue

            print(f"[{i}/{len(test_files)}] Running: {test_name}...", end=' ', flush=True)

            result = self.run_test(test_path)
            self.results.append(result)

            print(result.status)

            if result.status == "PASS" or result.status == "PASS_EQUIVALENT":
                passed += 1
            elif result.status == "SKIP":
                skipped += 1
            elif result.status == "ERROR":
                errors += 1
            else:
                failed += 1
                if result.diff:
                    print(f"  Diff:\n{result.diff[:500]}")

        print(f"\n{'='*60}")
        print(f"Results: {passed} passed, {failed} failed, {skipped} skipped, {errors} errors")

        self._save_results()

    def _save_results(self):
        """Save results to JSON file."""
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        results_file = self.output_dir / f"fbt_results_{self.target}_{timestamp}.json"

        results_data = {
            'metadata': {
                'target': self.target,
                'mode': self.mode,
                'timestamp': timestamp,
                'fbt_path': str(self.fbt_path),
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
                    'tracker_id': r.tracker_id,
                    'title': r.title,
                    'status': r.status,
                    'duration_ms': r.duration_ms,
                    'error_message': r.error_message,
                    'skipped_reason': r.skipped_reason,
                }
                for r in self.results
            ]
        }

        results_file.write_text(json.dumps(results_data, indent=2))
        print(f"\nResults saved to: {results_file}")


def main():
    parser = argparse.ArgumentParser(description='Firebird FBT Test Runner')
    parser.add_argument('--fbt-path', type=Path,
                        default=Path('/fbt-repository'),
                        help='Path to FBT repository')
    parser.add_argument('--suite', default='all',
                        choices=['all', 'bugs', 'functional', 'functional.basic',
                                'functional.gtcs', 'functional.intfunc'],
                        help='Test suite to run')
    parser.add_argument('--target', default='original',
                        choices=['original', 'scratchbird'],
                        help='Target engine to test')
    parser.add_argument('--mode', default='firebird',
                        choices=['firebird', 'mysql', 'postgresql'],
                        help='ScratchBird emulation mode (if target=scratchbird)')
    parser.add_argument('--output-dir', type=Path, default=Path('/results'),
                        help='Output directory for results')
    parser.add_argument('--limit', type=int, default=None,
                        help='Limit number of tests to run')
    parser.add_argument('--exclusions', type=Path, default=None,
                        help='Custom exclusions file')

    args = parser.parse_args()

    # Ensure output directory exists
    args.output_dir.mkdir(parents=True, exist_ok=True)

    # Create and run
    runner = FBTRunner(
        fbt_path=args.fbt_path,
        output_dir=args.output_dir,
        target=args.target,
        mode=args.mode
    )

    runner.run_suite(
        suite=args.suite,
        limit=args.limit,
        exclusions=None  # Will load from FB50-exclude-list.txt
    )


if __name__ == '__main__':
    main()
