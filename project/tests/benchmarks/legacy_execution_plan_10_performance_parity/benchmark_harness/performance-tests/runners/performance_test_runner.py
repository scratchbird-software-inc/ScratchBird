#!/usr/bin/env python3
"""Performance micro-benchmark runner for ScratchBird benchmark suites."""

from __future__ import annotations

import argparse
import json
import os
import statistics
import sys
import time
from dataclasses import asdict, dataclass
from datetime import datetime
from pathlib import Path
from typing import Any, Iterable, List, Optional

PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).parent.parent))

from scenarios.performance_tests import get_all_tests


@dataclass
class PerformanceResult:
    name: str
    description: str
    status: str
    iterations: int
    duration_ms: float
    metric: str
    value: float
    p50_ms: Optional[float] = None
    p95_ms: Optional[float] = None
    p99_ms: Optional[float] = None
    error_message: str = ""


class DatabaseConnection:
    def __init__(self, engine: str, host: str, port: Optional[int], database: str, user: str, password: str):
        self.engine = engine
        self.host = host
        self.port = port
        self.database = database
        self.user = user
        self.password = password
        self.connection = None
        self.cursor = None
        self.connect()

    def connect(self) -> None:
        if self.engine == "scratchbird":
            import scratchbird

            self.connection = scratchbird.connect(
                host=self.host,
                port=self.port,
                database=self.database,
                user=self.user,
                password=self.password,
                protocol="native",
                front_door_mode="direct",
                sslmode=os.getenv("BENCHMARK_SCRATCHBIRD_SSLMODE", "require"),
            )
        elif self.engine == "postgresql":
            import psycopg2

            self.connection = psycopg2.connect(
                host=self.host,
                port=self.port,
                dbname=self.database,
                user=self.user,
                password=self.password,
            )
        elif self.engine == "mysql":
            import pymysql

            self.connection = pymysql.connect(
                host=self.host,
                port=self.port,
                database=self.database,
                user=self.user,
                password=self.password,
                charset="utf8mb4",
                autocommit=False,
            )
        elif self.engine == "firebird":
            import fdb

            self.connection = fdb.connect(
                host=self.host,
                port=self.port,
                database=self.database,
                user=self.user,
                password=self.password,
            )
        else:
            raise ValueError(f"Unsupported engine: {self.engine}")
        self.connection.autocommit = False
        self.cursor = self.connection.cursor()

    def execute(self, sql: str) -> None:
        self.cursor.execute(sql)

    def fetchall(self) -> list[Any]:
        try:
            return self.cursor.fetchall()
        except Exception:
            return []

    def commit(self) -> None:
        self.connection.commit()

    def rollback(self) -> None:
        try:
            self.connection.rollback()
        except Exception:
            pass

    def close(self) -> None:
        if self.connection is not None:
            self.connection.close()


def percentile(values: List[float], pct: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int(round((pct / 100.0) * (len(ordered) - 1)))))
    return ordered[index]


def execute_ignoring_errors(db: DatabaseConnection, statements: Iterable[str]) -> None:
    for statement in statements:
        try:
            db.execute(statement)
            db.commit()
        except Exception:
            db.rollback()


def setup_schema(db: DatabaseConnection) -> None:
    execute_ignoring_errors(db, ["DROP TABLE perf_test", "DROP TABLE orders"])
    db.execute("CREATE TABLE perf_test (id BIGINT PRIMARY KEY, data VARCHAR(64))")
    db.execute(
        "CREATE TABLE orders (order_id BIGINT PRIMARY KEY, order_date DATE, total DECIMAL(18,2), data VARCHAR(64))"
    )
    db.commit()


def timed_statements(db: DatabaseConnection, statements: List[str], fetch: bool = False) -> tuple[float, List[float]]:
    latencies: List[float] = []
    start = time.perf_counter()
    for statement in statements:
        op_start = time.perf_counter()
        db.execute(statement)
        if fetch:
            db.fetchall()
        db.commit()
        latencies.append((time.perf_counter() - op_start) * 1000.0)
    return (time.perf_counter() - start) * 1000.0, latencies


def run_performance(args: argparse.Namespace) -> List[PerformanceResult]:
    tests = {test.name: test for test in get_all_tests()}
    results: List[PerformanceResult] = []

    db = DatabaseConnection(args.engine, args.host, args.port, args.database, args.user, args.password)
    try:
        setup_schema(db)

        insert_count = min(tests["insert_throughput"].iterations, 1000)
        insert_statements = [
            f"INSERT INTO perf_test (id, data) VALUES ({i}, 'payload-{i}')"
            for i in range(1, insert_count + 1)
        ]
        duration_ms, latencies = timed_statements(db, insert_statements)
        results.append(
            PerformanceResult(
                name="insert_throughput",
                description=tests["insert_throughput"].description,
                status="passed",
                iterations=insert_count,
                duration_ms=duration_ms,
                metric="ops_per_sec",
                value=(insert_count / duration_ms * 1000.0) if duration_ms else 0.0,
                p50_ms=statistics.median(latencies) if latencies else 0.0,
                p95_ms=percentile(latencies, 95),
                p99_ms=percentile(latencies, 99),
            )
        )

        order_inserts = [
            f"INSERT INTO orders (order_id, order_date, total, data) VALUES ({i}, DATE '2026-01-01', {i * 1.25:.2f}, 'order-{i}')"
            for i in range(1, 501)
        ]
        timed_statements(db, order_inserts)

        lookup_count = min(tests["pk_lookup_latency"].iterations, 500)
        lookup_statements = [
            f"SELECT * FROM orders WHERE order_id = {(i % 500) + 1}"
            for i in range(lookup_count)
        ]
        duration_ms, latencies = timed_statements(db, lookup_statements, fetch=True)
        results.append(
            PerformanceResult(
                name="pk_lookup_latency",
                description=tests["pk_lookup_latency"].description,
                status="passed",
                iterations=lookup_count,
                duration_ms=duration_ms,
                metric="p50_p95_p99",
                value=statistics.median(latencies) if latencies else 0.0,
                p50_ms=statistics.median(latencies) if latencies else 0.0,
                p95_ms=percentile(latencies, 95),
                p99_ms=percentile(latencies, 99),
            )
        )

        range_count = min(tests["range_scan"].iterations, 100)
        range_statements = [
            "SELECT * FROM orders WHERE order_date BETWEEN DATE '2026-01-01' AND DATE '2026-12-31'"
            for _ in range(range_count)
        ]
        duration_ms, _ = timed_statements(db, range_statements, fetch=True)
        results.append(
            PerformanceResult(
                name="range_scan",
                description=tests["range_scan"].description,
                status="passed",
                iterations=range_count,
                duration_ms=duration_ms,
                metric="queries_per_sec",
                value=(range_count / duration_ms * 1000.0) if duration_ms else 0.0,
            )
        )

        agg_count = min(tests["aggregation_speed"].iterations, 100)
        agg_statements = ["SELECT COUNT(*), SUM(total), AVG(total) FROM orders" for _ in range(agg_count)]
        duration_ms, _ = timed_statements(db, agg_statements, fetch=True)
        results.append(
            PerformanceResult(
                name="aggregation_speed",
                description=tests["aggregation_speed"].description,
                status="passed",
                iterations=agg_count,
                duration_ms=duration_ms,
                metric="queries_per_sec",
                value=(agg_count / duration_ms * 1000.0) if duration_ms else 0.0,
            )
        )

        mixed_count = min(tests["concurrent_mixed"].iterations, 500)
        mixed_statements = []
        for i in range(1, mixed_count + 1):
            if i % 5 == 0:
                mixed_statements.append("SELECT COUNT(*) FROM perf_test")
            else:
                mixed_statements.append(f"INSERT INTO perf_test (id, data) VALUES ({insert_count + i}, 'mixed-{i}')")
        duration_ms, _ = timed_statements(db, mixed_statements, fetch=True)
        results.append(
            PerformanceResult(
                name="concurrent_mixed",
                description=tests["concurrent_mixed"].description,
                status="passed",
                iterations=mixed_count,
                duration_ms=duration_ms,
                metric="tps",
                value=(mixed_count / duration_ms * 1000.0) if duration_ms else 0.0,
            )
        )
    except Exception as exc:
        db.rollback()
        results.append(
            PerformanceResult(
                name="performance_runner",
                description="Performance runner execution",
                status="error",
                iterations=0,
                duration_ms=0.0,
                metric="error",
                value=0.0,
                error_message=str(exc),
            )
        )
    finally:
        db.close()

    connect_iterations = min(tests["connection_overhead"].iterations, 20)
    latencies: List[float] = []
    for _ in range(connect_iterations):
        start = time.perf_counter()
        conn = DatabaseConnection(args.engine, args.host, args.port, args.database, args.user, args.password)
        conn.close()
        latencies.append((time.perf_counter() - start) * 1000.0)
    results.append(
        PerformanceResult(
            name="connection_overhead",
            description=tests["connection_overhead"].description,
            status="passed",
            iterations=connect_iterations,
            duration_ms=sum(latencies),
            metric="ms_per_connection",
            value=statistics.mean(latencies) if latencies else 0.0,
            p50_ms=statistics.median(latencies) if latencies else 0.0,
            p95_ms=percentile(latencies, 95),
            p99_ms=percentile(latencies, 99),
        )
    )
    return results


def main() -> int:
    parser = argparse.ArgumentParser(description="Performance Test Runner")
    parser.add_argument("--engine", required=True, help="Database engine")
    parser.add_argument("--host", default="localhost", help="Database host")
    parser.add_argument("--port", type=int, help="Database port")
    parser.add_argument("--database", required=True, help="Database name")
    parser.add_argument("--user", required=True, help="Database user")
    parser.add_argument("--password", required=True, help="Database password")
    parser.add_argument("--output-dir", type=Path, default=Path("results"), help="Output directory")
    args = parser.parse_args()

    print(f"Running performance micro-benchmarks for {args.engine}")
    results = run_performance(args)
    passed = sum(1 for result in results if result.status == "passed")
    failed = sum(1 for result in results if result.status == "failed")
    errors = sum(1 for result in results if result.status == "error")

    payload = {
        "metadata": {
            "engine": args.engine,
            "suite": "performance",
            "timestamp": datetime.now().isoformat(),
            "host": args.host,
            "benchmark_profile": "bounded_micro_profile",
        },
        "results": [asdict(result) for result in results],
        "summary": {
            "total_tests": len(results),
            "passed": passed,
            "failed": failed,
            "errors": errors,
        },
    }

    args.output_dir.mkdir(parents=True, exist_ok=True)
    output_file = args.output_dir / f"performance-{args.engine}-{datetime.now():%Y%m%d-%H%M%S}.json"
    output_file.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    print(f"Results saved to: {output_file}")
    return 0 if failed == 0 and errors == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
