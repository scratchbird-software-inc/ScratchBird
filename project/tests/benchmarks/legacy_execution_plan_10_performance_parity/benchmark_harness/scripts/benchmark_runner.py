#!/usr/bin/env python3
"""
ScratchBird Benchmark Runner

Orchestrates benchmark execution across multiple database engines.
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, asdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, List, Optional


@dataclass
class BenchmarkResult:
    """Single benchmark test result"""
    test_name: str
    engine: str
    duration_ms: float
    iterations: int
    rows_affected: int
    error: Optional[str] = None


@dataclass
class EngineConnection:
    """Database engine connection info"""
    name: str
    host: str
    port: int
    database: str
    user: str
    password: str


class EngineConnector:
    """Base class for database engine connections"""

    def __init__(self, conn_info: EngineConnection):
        self.conn_info = conn_info
        self.connection = None

    def connect(self):
        raise NotImplementedError

    def execute(self, sql: str, params=None) -> tuple:
        """Execute SQL and return (row_count, error)"""
        raise NotImplementedError

    def close(self):
        if self.connection:
            self.connection.close()


class FirebirdConnector(EngineConnector):
    """FirebirdSQL connector using fdb"""

    def connect(self):
        import fdb
        self.connection = fdb.connect(
            host=self.conn_info.host,
            port=self.conn_info.port,
            database=self.conn_info.database,
            user=self.conn_info.user,
            password=self.conn_info.password
        )

    def execute(self, sql: str, params=None):
        try:
            cursor = self.connection.cursor()
            cursor.execute(sql, params or ())
            row_count = cursor.rowcount
            self.connection.commit()
            return row_count, None
        except Exception as e:
            return 0, str(e)


class MySQLConnector(EngineConnector):
    """MySQL connector using pymysql"""

    def connect(self):
        import pymysql
        self.connection = pymysql.connect(
            host=self.conn_info.host,
            port=self.conn_info.port,
            database=self.conn_info.database,
            user=self.conn_info.user,
            password=self.conn_info.password,
            autocommit=True
        )

    def execute(self, sql: str, params=None):
        try:
            with self.connection.cursor() as cursor:
                cursor.execute(sql, params or ())
                row_count = cursor.rowcount
                return row_count, None
        except Exception as e:
            return 0, str(e)


class PostgreSQLConnector(EngineConnector):
    """PostgreSQL connector using psycopg2"""

    def connect(self):
        import psycopg2
        self.connection = psycopg2.connect(
            host=self.conn_info.host,
            port=self.conn_info.port,
            dbname=self.conn_info.database,
            user=self.conn_info.user,
            password=self.conn_info.password
        )

    def execute(self, sql: str, params=None):
        try:
            cursor = self.connection.cursor()
            cursor.execute(sql, params or ())
            row_count = cursor.rowcount
            self.connection.commit()
            return row_count, None
        except Exception as e:
            return 0, str(e)


def _harness_root() -> Path:
    return Path(__file__).resolve().parents[1]


def _repo_root() -> Optional[Path]:
    for candidate in Path(__file__).resolve().parents:
        if (candidate / "project" / "drivers" / "driver" / "python" / "src" / "scratchbird").exists():
            return candidate
    return None


def _read_env_file(path: Path) -> Dict[str, str]:
    values: Dict[str, str] = {}
    if not path.exists():
        return values
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key.strip()] = value.strip().strip('"').strip("'")
    return values


def _scratchbird_env() -> Dict[str, str]:
    values = _read_env_file(_harness_root() / ".benchmark-engine-ports" / "scratchbird.env")
    for key in (
        "BENCHMARK_SCRATCHBIRD_HOST",
        "BENCHMARK_SCRATCHBIRD_PORT",
        "BENCHMARK_SCRATCHBIRD_DB",
        "BENCHMARK_SCRATCHBIRD_USER",
        "BENCHMARK_SCRATCHBIRD_PASSWORD",
        "BENCHMARK_SCRATCHBIRD_SSLMODE",
        "BENCHMARK_SCRATCHBIRD_SCRIPT_INPUT_DIR",
        "BENCHMARK_SCRATCHBIRD_SCRIPT_OUTPUT_DIR",
        "BENCHMARK_SCRATCHBIRD_MONITOR_JSONL",
        "SCRATCHBIRD_SB_ISQL",
        "SCRATCHBIRD_DRIVER_PYTHONPATH",
    ):
        if key in os.environ:
            values[key] = os.environ[key]
    return values


def ensure_scratchbird_driver() -> None:
    candidates: List[Path] = []
    env_values = _scratchbird_env()
    env_driver_path = env_values.get("SCRATCHBIRD_DRIVER_PYTHONPATH")
    if env_driver_path:
        candidates.append(Path(env_driver_path).expanduser())

    repo_root = _repo_root()
    if repo_root is not None:
        candidates.append(repo_root / "project" / "drivers" / "driver" / "python" / "src")

    for root_candidate in _harness_root().parents:
        candidates.append(
            root_candidate
            / "ScratchBird-driver"
            / "tracks"
            / "p3"
            / "drivers"
            / "python"
            / "src"
        )

    for candidate in candidates:
        if candidate.exists():
            sys.path.insert(0, str(candidate))
            return


class ScratchBirdConnector(EngineConnector):
    """ScratchBird connector that drives sb_isql scripts against the native route."""

    def connect(self):
        env_values = _scratchbird_env()
        self.sb_isql = self._resolve_sb_isql(env_values)
        self.sslmode = env_values.get("BENCHMARK_SCRATCHBIRD_SSLMODE", "require")
        self.work_dir = Path(tempfile.mkdtemp(prefix="sb_bench_isql_"))
        self.input_dir = Path(env_values.get(
            "BENCHMARK_SCRATCHBIRD_SCRIPT_INPUT_DIR",
            str(self.work_dir / "input"),
        )).expanduser()
        self.output_dir = Path(env_values.get(
            "BENCHMARK_SCRATCHBIRD_SCRIPT_OUTPUT_DIR",
            str(self.work_dir / "output"),
        )).expanduser()
        monitor_jsonl = env_values.get("BENCHMARK_SCRATCHBIRD_MONITOR_JSONL")
        self.monitor_jsonl = Path(monitor_jsonl).expanduser() if monitor_jsonl else self.work_dir / "monitor.jsonl"
        self.input_dir.mkdir(parents=True, exist_ok=True)
        self.output_dir.mkdir(parents=True, exist_ok=True)
        self.monitor_jsonl.parent.mkdir(parents=True, exist_ok=True)

    def _resolve_sb_isql(self, env_values: Dict[str, str]) -> Path:
        candidates: List[Path] = []
        if env_values.get("SCRATCHBIRD_SB_ISQL"):
            candidates.append(Path(env_values["SCRATCHBIRD_SB_ISQL"]).expanduser())
        repo_root = _repo_root()
        if repo_root is not None:
            candidates.extend(
                [
                    repo_root / "build" / "drivers" / "tool" / "cli" / "cmake" / "sb_isql",
                    repo_root / "build" / "drivers" / "tool" / "cli" / "sb_isql",
                    repo_root / "build" / "drivers" / "tool" / "cli" / "src" / "sb_isql",
                ]
            )
        for candidate in candidates:
            if candidate.exists() and os.access(candidate, os.X_OK):
                return candidate
        raise RuntimeError("ScratchBird sb_isql binary was not found")

    def _base_command(self) -> List[str]:
        return [
            str(self.sb_isql),
            self.conn_info.database,
            "--mode=inet",
            "--front-door-mode=direct",
            f"--host={self.conn_info.host}",
            f"--port={self.conn_info.port}",
            f"--sslmode={self.sslmode}",
            "--conn-opt",
            "enable_copy_streaming=true",
            "-U",
            self.conn_info.user,
            "-P",
            self.conn_info.password,
            "-q",
            "-b",
            "-A",
            "-t",
        ]

    def _emit_monitor_event(self, event: Dict[str, object]) -> None:
        event.setdefault("timestamp", datetime.now(timezone.utc).replace(microsecond=0).isoformat())
        event.setdefault("engine", "scratchbird")
        with self.monitor_jsonl.open("a", encoding="utf-8") as handle:
            handle.write(json.dumps(event, sort_keys=True) + "\n")
            handle.flush()

    def _run_script(self, name: str, statements: List[str]) -> tuple[int, Optional[str]]:
        script_path = self.input_dir / f"{name}.sql"
        stdout_path = self.output_dir / f"{name}.stdout"
        stderr_path = self.output_dir / f"{name}.stderr"
        result_path = self.output_dir / f"{name}.result.json"
        script_text = (
            "SET COUNT OFF;\n"
            "SET HEADING OFF;\n"
            "BEGIN;\n" +
            "\n".join(statements) +
            "\nCOMMIT;\n"
        )
        script_path.write_text(script_text, encoding="utf-8")
        command = self._base_command() + ["-f", str(script_path)]
        self._emit_monitor_event({
            "event": "script_started",
            "name": name,
            "script_path": str(script_path),
            "stdout_path": str(stdout_path),
            "stderr_path": str(stderr_path),
            "result_path": str(result_path),
            "statement_count": len(statements),
        })
        start = time.perf_counter()
        with stdout_path.open("w", encoding="utf-8") as stdout_handle, \
                stderr_path.open("w", encoding="utf-8") as stderr_handle:
            completed = subprocess.run(
                command,
                text=True,
                stdout=stdout_handle,
                stderr=stderr_handle,
                timeout=int(os.environ.get("BENCHMARK_SCRATCHBIRD_ISQL_TIMEOUT", "120")),
            )
        duration_ms = (time.perf_counter() - start) * 1000
        stdout_text = stdout_path.read_text(encoding="utf-8", errors="replace")
        stderr_text = stderr_path.read_text(encoding="utf-8", errors="replace")
        if completed.returncode != 0:
            output = (stdout_text + "\n" + stderr_text).strip()
            result_payload = {
                "name": name,
                "returncode": completed.returncode,
                "duration_ms": duration_ms,
                "rows": 0,
                "error": output or f"sb_isql exited with {completed.returncode}",
                "script_path": str(script_path),
                "stdout_path": str(stdout_path),
                "stderr_path": str(stderr_path),
            }
            result_path.write_text(json.dumps(result_payload, indent=2), encoding="utf-8")
            self._emit_monitor_event({"event": "script_failed", **result_payload})
            return 0, result_payload["error"]
        rows = 0
        for line in stdout_text.splitlines():
            line = line.strip()
            if line.startswith("COPY ") and " rows " in line:
                parts = line.split()
                if len(parts) >= 2 and parts[1].lstrip("-").isdigit():
                    rows += int(parts[1])
        result_payload = {
            "name": name,
            "returncode": completed.returncode,
            "duration_ms": duration_ms,
            "rows": rows,
            "error": None,
            "script_path": str(script_path),
            "stdout_path": str(stdout_path),
            "stderr_path": str(stderr_path),
        }
        result_path.write_text(json.dumps(result_payload, indent=2), encoding="utf-8")
        self._emit_monitor_event({"event": "script_completed", **result_payload})
        return rows, None

    def _copy_statement(self, sql: str, params: bytes, name: str) -> str:
        table = sql.strip()
        upper = table.upper()
        if not upper.startswith("COPY ") or " FROM STDIN" not in upper:
            raise ValueError(f"unsupported ScratchBird COPY benchmark SQL: {sql}")
        table = table[5:upper.index(" FROM STDIN")].strip()
        data_path = self.input_dir / f"{name}.copy"
        data_path.write_bytes(params)
        self._emit_monitor_event({
            "event": "copy_input_written",
            "name": name,
            "copy_path": str(data_path),
            "byte_count": len(params),
        })
        return f"\\copy {table} FROM '{data_path}'"

    def execute(self, sql: str, params=None):
        try:
            if sql.strip().upper().startswith("COPY "):
                data = params if isinstance(params, bytes) else str(params or "").encode("utf-8")
                statement = self._copy_statement(sql, data, f"single_{time.time_ns()}")
            else:
                statement = sql.rstrip(";") + ";"
            return self._run_script(f"single_{time.time_ns()}", [statement])
        except Exception as e:
            return 0, str(e)

    def execute_benchmark(self, test_name: str, sql: str, params_template: Optional[str],
                          iterations: int, unique_seed: int) -> tuple[int, Optional[str], float]:
        try:
            warmup_count = min(10, iterations)
            warmup = []
            benchmark = []
            rendered_iteration = 0
            for _ in range(warmup_count):
                rendered_sql = render_benchmark_sql(sql, rendered_iteration, unique_seed)
                rendered_params = render_benchmark_params(params_template, rendered_iteration, unique_seed)
                if rendered_sql.strip().upper().startswith("COPY "):
                    warmup.append(self._copy_statement(rendered_sql, rendered_params or b"", f"{test_name}_warmup_{rendered_iteration}"))
                else:
                    warmup.append(rendered_sql.rstrip(";") + ";")
                rendered_iteration += 1
            if warmup:
                _, err = self._run_script(f"{test_name}_warmup", warmup)
                if err:
                    return 0, err, 0.0

            for _ in range(iterations):
                rendered_sql = render_benchmark_sql(sql, rendered_iteration, unique_seed)
                rendered_params = render_benchmark_params(params_template, rendered_iteration, unique_seed)
                if rendered_sql.strip().upper().startswith("COPY "):
                    benchmark.append(self._copy_statement(rendered_sql, rendered_params or b"", f"{test_name}_{rendered_iteration}"))
                else:
                    benchmark.append(rendered_sql.rstrip(";") + ";")
                rendered_iteration += 1
            start = time.perf_counter()
            rows, err = self._run_script(test_name, benchmark)
            duration_ms = (time.perf_counter() - start) * 1000
            return rows, err, duration_ms
        except Exception as e:
            return 0, str(e), 0.0


_SCRATCHBIRD_ENV = _scratchbird_env()


# Engine connection configurations
ENGINE_CONFIGS = {
    "firebird": EngineConnection(
        name="firebird",
        host="firebird",
        port=3050,
        database="/firebird/data/benchmark.fdb",
        user="benchmark",
        password="benchmark"
    ),
    "mysql": EngineConnection(
        name="mysql",
        host="mysql",
        port=3306,
        database="benchmark",
        user="benchmark",
        password="benchmark"
    ),
    "postgresql": EngineConnection(
        name="postgresql",
        host="postgresql",
        port=5432,
        database="benchmark",
        user="benchmark",
        password="benchmark"
    ),
    "scratchbird": EngineConnection(
        name="scratchbird",
        host=_SCRATCHBIRD_ENV.get("BENCHMARK_SCRATCHBIRD_HOST", "127.0.0.1"),
        port=int(_SCRATCHBIRD_ENV.get("BENCHMARK_SCRATCHBIRD_PORT", "3092")),
        database=_SCRATCHBIRD_ENV.get("BENCHMARK_SCRATCHBIRD_DB", "main"),
        user=_SCRATCHBIRD_ENV.get("BENCHMARK_SCRATCHBIRD_USER", "SysArch"),
        password=_SCRATCHBIRD_ENV.get("BENCHMARK_SCRATCHBIRD_PASSWORD", "replaceme"),
    ),
}


def get_connector(engine_name: str) -> EngineConnector:
    """Get appropriate connector for engine"""
    config = ENGINE_CONFIGS[engine_name]
    connectors = {
        "firebird": FirebirdConnector,
        "mysql": MySQLConnector,
        "postgresql": PostgreSQLConnector,
        "scratchbird": ScratchBirdConnector,
    }
    return connectors[engine_name](config)


def render_benchmark_sql(sql: str, iteration: int, unique_seed: int) -> str:
    """Render per-iteration placeholders used by dialects without sequences."""
    if "{iteration}" not in sql and "{unique_id}" not in sql:
        return sql
    return sql.format(iteration=iteration, unique_id=unique_seed + iteration)


def render_benchmark_params(template: Optional[str], iteration: int, unique_seed: int):
    if template is None:
        return None
    return template.format(iteration=iteration, unique_id=unique_seed + iteration).encode("utf-8")


# Micro-benchmark test definitions
MICRO_BENCHMARKS = {
    "single_insert": {
        "description": "Single row INSERT performance",
        "sql": {
            "firebird": "INSERT INTO perf_test (id, name, value) VALUES (GEN_ID(gen_perf_test, 1), 'test', 123.45)",
            "mysql": "INSERT INTO perf_test (name, value) VALUES ('test', 123.45)",
            "postgresql": "INSERT INTO perf_test (name, value) VALUES ('test', 123.45)",
            "scratchbird": "COPY users.public.sbsfc021_stream_table FROM STDIN",
        },
        "copy_data": {
            "scratchbird": "id={unique_id};payload=benchmark\n",
        },
        "iterations": 1000,
    },
    "point_select": {
        "description": "Primary key lookup",
        "sql": {
            "firebird": "SELECT * FROM perf_test WHERE id = 1",
            "mysql": "SELECT * FROM perf_test WHERE id = 1",
            "postgresql": "SELECT * FROM perf_test WHERE id = 1",
            "scratchbird": "SELECT * FROM users.public.sbsfc021_stream_table WHERE id = '6'",
        },
        "iterations": 1000,
    },
    "simple_aggregate": {
        "description": "Simple COUNT(*) aggregation",
        "sql": {
            "firebird": "SELECT COUNT(*) FROM perf_test",
            "mysql": "SELECT COUNT(*) FROM perf_test",
            "postgresql": "SELECT COUNT(*) FROM perf_test",
            "scratchbird": "SELECT COUNT(*) FROM users.public.sbsfc021_stream_table",
        },
        "iterations": 100,
    },
}


SCHEMA_SQL = {
    "firebird": """
            CREATE GENERATOR gen_perf_test IF NOT EXISTS;
            CREATE TABLE perf_test (
                id INTEGER PRIMARY KEY,
                name VARCHAR(100),
                value DOUBLE PRECISION,
                created TIMESTAMP DEFAULT CURRENT_TIMESTAMP
            );
        """,
    "mysql": """
            CREATE TABLE IF NOT EXISTS perf_test (
                id INTEGER AUTO_INCREMENT PRIMARY KEY,
                name VARCHAR(100),
                value DOUBLE,
                created TIMESTAMP DEFAULT CURRENT_TIMESTAMP
            );
        """,
    "postgresql": """
            CREATE TABLE IF NOT EXISTS perf_test (
                id SERIAL PRIMARY KEY,
                name VARCHAR(100),
                value DOUBLE PRECISION,
                created TIMESTAMP DEFAULT CURRENT_TIMESTAMP
            );
        """,
    "scratchbird": "",
}


def setup_schema(connector: EngineConnector, engine: str):
    """Create benchmark schema"""
    sql = SCHEMA_SQL.get(engine, SCHEMA_SQL["postgresql"])
    for statement in sql.split(';'):
        statement = statement.strip()
        if statement:
            connector.execute(statement)


def run_micro_benchmarks(engines: List[str], iteration_limit: Optional[int] = None) -> List[BenchmarkResult]:
    """Run micro-benchmark suite"""
    results = []

    for engine in engines:
        print(f"\nRunning micro-benchmarks on {engine}...")

        try:
            connector = get_connector(engine)
            connector.connect()

            # Setup schema
            setup_schema(connector, engine)

            # Run each benchmark
            for test_name, test_config in MICRO_BENCHMARKS.items():
                sql = test_config["sql"].get(engine, test_config["sql"]["postgresql"])
                params_template = test_config.get("copy_data", {}).get(engine)
                iterations = test_config["iterations"]
                if iteration_limit is not None and iteration_limit > 0:
                    iterations = min(iterations, iteration_limit)
                unique_seed = 1000000 if engine == "scratchbird" else int(time.time() * 1000000)
                if hasattr(connector, "execute_benchmark"):
                    rows_total, error, duration_ms = connector.execute_benchmark(
                        test_name,
                        sql,
                        params_template,
                        iterations,
                        unique_seed,
                    )
                    result = BenchmarkResult(
                        test_name=test_name,
                        engine=engine,
                        duration_ms=duration_ms,
                        iterations=iterations,
                        rows_affected=rows_total,
                        error=error
                    )
                    results.append(result)
                    status = "✓" if not error else "✗"
                    print(f"  {status} {test_name}: {duration_ms:.2f}ms ({iterations} iterations)")
                    continue

                rendered_iteration = 0

                # Warmup
                for _ in range(min(10, iterations)):
                    connector.execute(
                        render_benchmark_sql(sql, rendered_iteration, unique_seed),
                        render_benchmark_params(params_template, rendered_iteration, unique_seed),
                    )
                    rendered_iteration += 1

                # Benchmark
                start = time.perf_counter()
                error = None
                rows_total = 0

                for _ in range(iterations):
                    rows, err = connector.execute(
                        render_benchmark_sql(sql, rendered_iteration, unique_seed),
                        render_benchmark_params(params_template, rendered_iteration, unique_seed),
                    )
                    rendered_iteration += 1
                    if err:
                        error = err
                        break
                    rows_total += rows

                duration_ms = (time.perf_counter() - start) * 1000

                result = BenchmarkResult(
                    test_name=test_name,
                    engine=engine,
                    duration_ms=duration_ms,
                    iterations=iterations,
                    rows_affected=rows_total,
                    error=error
                )
                results.append(result)

                status = "✓" if not error else "✗"
                print(f"  {status} {test_name}: {duration_ms:.2f}ms ({iterations} iterations)")

            connector.close()

        except Exception as e:
            print(f"  ✗ Failed to connect to {engine}: {e}")

    return results


def main():
    parser = argparse.ArgumentParser(description="ScratchBird Benchmark Runner")
    parser.add_argument("--suite", choices=["micro", "concurrent", "regression", "all"],
                        default="micro", help="Benchmark suite to run")
    parser.add_argument("--engine", default="all",
                        help="Engine to test (firebird,mysql,postgresql,scratchbird, or all)")
    parser.add_argument("--output", default="./results/benchmark.json",
                        help="Output file for results")
    parser.add_argument("--iteration-limit", type=int, default=None,
                        help="Optional per-test iteration cap for live smoke/regression runs")
    parser.add_argument("--fail-on-error", action="store_true",
                        help="Return non-zero when any benchmark fails or no benchmark result is produced")
    parser.add_argument("--scratchbird-script-input-dir", default=None,
                        help="Directory for generated ScratchBird sb_isql SQL and COPY input files")
    parser.add_argument("--scratchbird-script-output-dir", default=None,
                        help="Directory for generated ScratchBird sb_isql stdout/stderr/result files")
    parser.add_argument("--scratchbird-monitor-jsonl", default=None,
                        help="JSONL event stream for monitoring ScratchBird sb_isql benchmark progress")

    args = parser.parse_args()
    if args.scratchbird_script_input_dir:
        os.environ["BENCHMARK_SCRATCHBIRD_SCRIPT_INPUT_DIR"] = args.scratchbird_script_input_dir
    if args.scratchbird_script_output_dir:
        os.environ["BENCHMARK_SCRATCHBIRD_SCRIPT_OUTPUT_DIR"] = args.scratchbird_script_output_dir
    if args.scratchbird_monitor_jsonl:
        os.environ["BENCHMARK_SCRATCHBIRD_MONITOR_JSONL"] = args.scratchbird_monitor_jsonl

    # Determine engines to test
    if args.engine == "all":
        engines = ["firebird", "mysql", "postgresql", "scratchbird"]
    else:
        engines = args.engine.split(",")

    print("ScratchBird Benchmark Runner")
    print("=" * 50)
    print(f"Suite: {args.suite}")
    print(f"Engines: {', '.join(engines)}")
    print(f"Output: {args.output}")
    print()

    # Run benchmarks
    all_results = []

    if args.suite in ["micro", "all"]:
        results = run_micro_benchmarks(engines, args.iteration_limit)
        all_results.extend(results)

    # Generate report
    report = {
        "timestamp": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
        "suite": args.suite,
        "engines": engines,
        "results": [asdict(r) for r in all_results],
    }

    # Save results
    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    with open(output_path, 'w') as f:
        json.dump(report, f, indent=2)

    print(f"\nResults saved to: {args.output}")

    # Print summary
    print("\nSummary:")
    print("-" * 50)
    for engine in engines:
        engine_results = [r for r in all_results if r.engine == engine]
        passed = sum(1 for r in engine_results if not r.error)
        failed = len(engine_results) - passed
        print(f"  {engine}: {passed} passed, {failed} failed")

    if args.fail_on_error and (not all_results or any(r.error for r in all_results)):
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
