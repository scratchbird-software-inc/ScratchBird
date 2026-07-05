#!/usr/bin/env python3
"""Bounded TPC-H profile runner for ScratchBird benchmarks.

This executes the repository's TPC-H query definitions as an analytical SQL
profile. It is not a certified TPC-H publication harness.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from dataclasses import asdict, dataclass
from datetime import datetime
from pathlib import Path
from typing import Optional

PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).parent.parent))

from scenarios.tpc_h_queries import TPCHQueries, get_schema_sql, get_seed_sql


@dataclass
class QueryResult:
    query_num: int
    name: str
    description: str
    duration_ms: float
    rows_returned: int
    status: str
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

    def execute(self, sql: str) -> list[tuple]:
        self.cursor.execute(sql)
        try:
            rows = self.cursor.fetchall()
        except Exception:
            rows = []
        self.connection.commit()
        return rows

    def rollback(self) -> None:
        try:
            self.connection.rollback()
        except Exception:
            pass

    def close(self) -> None:
        if self.connection is not None:
            self.connection.close()


def split_sql_script(script: str) -> list[str]:
    statements = []
    current = []
    for raw_line in script.splitlines():
        line = raw_line.strip()
        if not line or line.startswith("--"):
            continue
        current.append(raw_line)
        if line.endswith(";"):
            statement = "\n".join(current).strip().rstrip(";").strip()
            if statement:
                statements.append(statement)
            current = []
    tail = "\n".join(current).strip()
    if tail:
        statements.append(tail)
    return statements


def setup_schema(db: DatabaseConnection) -> None:
    for table in ["lineitem", "orders", "customer", "partsupp", "supplier", "part", "nation", "region"]:
        try:
            db.execute(f"DROP TABLE {table}")
        except Exception:
            db.rollback()
    for statement in split_sql_script(get_schema_sql()):
        db.execute(statement)
    for statement in get_seed_sql():
        db.execute(statement)


def run_queries(db: DatabaseConnection) -> list[QueryResult]:
    results: list[QueryResult] = []
    for query in TPCHQueries.get_all_queries():
        start = time.perf_counter()
        status = "passed"
        error = ""
        rows_returned = 0
        try:
            rows = db.execute(query.sql)
            rows_returned = len(rows)
        except Exception as exc:
            db.rollback()
            status = "error"
            error = str(exc)
        duration_ms = (time.perf_counter() - start) * 1000.0
        results.append(
            QueryResult(
                query_num=query.num,
                name=query.name,
                description=query.description,
                duration_ms=duration_ms,
                rows_returned=rows_returned,
                status=status,
                error_message=error,
            )
        )
    return results


def main() -> int:
    parser = argparse.ArgumentParser(description="TPC-H Benchmark Runner")
    parser.add_argument("--engine", required=True, help="Database engine")
    parser.add_argument("--host", default="localhost", help="Database host")
    parser.add_argument("--port", type=int, help="Database port")
    parser.add_argument("--database", required=True, help="Database name")
    parser.add_argument("--user", required=True, help="Database user")
    parser.add_argument("--password", required=True, help="Database password")
    parser.add_argument("--scale", type=int, default=1, help="Scale factor")
    parser.add_argument("--output-dir", type=Path, default=Path("results"), help="Output directory")
    args = parser.parse_args()

    print(f"Running bounded TPC-H profile for {args.engine}")
    db = DatabaseConnection(args.engine, args.host, args.port, args.database, args.user, args.password)
    try:
        setup_schema(db)
        results = run_queries(db)
    finally:
        db.close()

    passed = sum(1 for result in results if result.status == "passed")
    errors = sum(1 for result in results if result.status == "error")
    payload = {
        "metadata": {
            "engine": args.engine,
            "suite": "tpc-h",
            "timestamp": datetime.now().isoformat(),
            "scale_factor": args.scale,
            "host": args.host,
            "benchmark_profile": "bounded_tpch_query_profile",
            "certification_status": "not_certified_tpc_h_publication",
        },
        "results": [asdict(result) for result in results],
        "summary": {
            "total_tests": len(results),
            "passed": passed,
            "failed": 0,
            "errors": errors,
        },
    }

    args.output_dir.mkdir(parents=True, exist_ok=True)
    output_file = args.output_dir / f"tpc-h-{args.engine}-{datetime.now():%Y%m%d-%H%M%S}.json"
    output_file.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    print(f"Results saved to: {output_file}")
    return 0 if errors == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
