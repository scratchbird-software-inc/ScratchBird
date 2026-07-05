#!/usr/bin/env python3
"""Bounded TPC-C profile runner for ScratchBird benchmarks.

This is not a certified TPC-C publication harness. It executes the repository's
TPC-C transaction definitions as a repeatable SQL profile so connection, parser,
transaction, and execution regressions are measured directly.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time
from dataclasses import asdict, dataclass
from datetime import datetime
from pathlib import Path
from typing import Any, Optional

PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).parent.parent))

from scenarios.tpc_c_workload import TPCCWorkload, get_schema_sql


@dataclass
class TransactionResult:
    name: str
    weight: int
    statements: int
    executions: int
    duration_ms: float
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

    def execute(self, sql: str) -> None:
        self.cursor.execute(sql)
        try:
            self.cursor.fetchall()
        except Exception:
            pass

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


def sql_literal(value: Any) -> str:
    if isinstance(value, str):
        return "'" + value.replace("'", "''") + "'"
    return str(value)


VALUE_MAP = {
    "w_id": 1,
    "d_id": 1,
    "c_id": 1,
    "last": "BARBARBAR",
    "amount": 10.00,
    "o_id": 1,
    "ol_cnt": 5,
    "all_local": 1,
    "i_id": 1,
    "qty": 1,
    "supply_w_id": 1,
    "ol_number": 1,
    "dist_info": "DIST-INFO",
    "carrier_id": 1,
    "total_amount": 10.00,
    "o_id_low": 1,
    "o_id_high": 20,
    "threshold": 15,
    "data": "TPC-C profile",
}


def bind_profile_values(sql: str) -> str:
    def replace(match: re.Match[str]) -> str:
        name = match.group(1)
        return sql_literal(VALUE_MAP.get(name, 1))

    return re.sub(r":([A-Za-z_][A-Za-z0-9_]*)", replace, sql)


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
    for table in ["history", "order_line", "new_order", "orders", "stock", "customer", "district", "warehouse", "item"]:
        try:
            db.execute(f"DROP TABLE {table}")
            db.commit()
        except Exception:
            db.rollback()
    for statement in split_sql_script(get_schema_sql()):
        db.execute(statement)
        db.commit()
    for statement in seed_sql():
        db.execute(statement)
        db.commit()


def seed_sql() -> list[str]:
    timestamp = "TIMESTAMP '2026-01-01 00:00:00'"
    return [
        "INSERT INTO warehouse (w_id, w_name, w_street_1, w_street_2, w_city, w_state, w_zip, w_tax, w_ytd) VALUES (1, 'W1', 'street1', 'street2', 'city', 'ON', 'A1A1A1A1A', 0.0500, 0.00)",
        "INSERT INTO district (d_id, d_w_id, d_name, d_street_1, d_street_2, d_city, d_state, d_zip, d_tax, d_ytd, d_next_o_id) VALUES (1, 1, 'D1', 'street1', 'street2', 'city', 'ON', 'A1A1A1A1A', 0.0500, 0.00, 2)",
        f"INSERT INTO customer (c_id, c_d_id, c_w_id, c_first, c_middle, c_last, c_street_1, c_street_2, c_city, c_state, c_zip, c_phone, c_since, c_credit, c_credit_lim, c_discount, c_balance, c_ytd_payment, c_payment_cnt, c_delivery_cnt, c_data) VALUES (1, 1, 1, 'FIRST', 'OE', 'BARBARBAR', 'street1', 'street2', 'city', 'ON', 'A1A1A1A1A', '5555555555555555', {timestamp}, 'GC', 50000.00, 0.0500, 0.00, 0.00, 0, 0, 'customer')",
        f"INSERT INTO orders (o_id, o_d_id, o_w_id, o_c_id, o_entry_d, o_carrier_id, o_ol_cnt, o_all_local) VALUES (1, 1, 1, 1, {timestamp}, 1, 1, 1)",
        f"INSERT INTO order_line (ol_o_id, ol_d_id, ol_w_id, ol_number, ol_i_id, ol_supply_w_id, ol_delivery_d, ol_quantity, ol_amount, ol_dist_info) VALUES (1, 1, 1, 1, 1, 1, {timestamp}, 1, 10.00, 'DIST-INFO')",
        "INSERT INTO new_order (no_o_id, no_d_id, no_w_id) VALUES (1, 1, 1)",
        "INSERT INTO item (i_id, i_im_id, i_name, i_price, i_data) VALUES (1, 1, 'ITEM1', 10.00, 'item')",
        "INSERT INTO stock (s_i_id, s_w_id, s_quantity, s_dist_01, s_dist_02, s_dist_03, s_dist_04, s_dist_05, s_dist_06, s_dist_07, s_dist_08, s_dist_09, s_dist_10, s_ytd, s_order_cnt, s_remote_cnt, s_data) VALUES (1, 1, 100, 'D01', 'D02', 'D03', 'D04', 'D05', 'D06', 'D07', 'D08', 'D09', 'D10', 0, 0, 0, 'stock')",
    ]


def run_transactions(db: DatabaseConnection, warehouses: int) -> list[TransactionResult]:
    results: list[TransactionResult] = []
    repetitions = max(1, min(warehouses, 3))
    for transaction in TPCCWorkload.get_all_transactions():
        start = time.perf_counter()
        executions = 0
        status = "passed"
        error = ""
        try:
            for _ in range(repetitions):
                for statement in transaction.sql_statements:
                    db.execute(bind_profile_values(statement))
                    executions += 1
                db.commit()
        except Exception as exc:
            db.rollback()
            status = "error"
            error = str(exc)
        duration_ms = (time.perf_counter() - start) * 1000.0
        results.append(
            TransactionResult(
                name=transaction.name,
                weight=transaction.weight,
                statements=len(transaction.sql_statements),
                executions=executions,
                duration_ms=duration_ms,
                status=status,
                error_message=error,
            )
        )
    return results


def main() -> int:
    parser = argparse.ArgumentParser(description="TPC-C Benchmark Runner")
    parser.add_argument("--engine", required=True, help="Database engine")
    parser.add_argument("--host", default="localhost", help="Database host")
    parser.add_argument("--port", type=int, help="Database port")
    parser.add_argument("--database", required=True, help="Database name")
    parser.add_argument("--user", required=True, help="Database user")
    parser.add_argument("--password", required=True, help="Database password")
    parser.add_argument("--warehouses", type=int, default=1, help="Number of warehouses")
    parser.add_argument("--duration", type=int, default=300, help="Declared benchmark duration in seconds")
    parser.add_argument("--output-dir", type=Path, default=Path("results"), help="Output directory")
    args = parser.parse_args()

    print(f"Running bounded TPC-C profile for {args.engine}")
    db = DatabaseConnection(args.engine, args.host, args.port, args.database, args.user, args.password)
    try:
        setup_schema(db)
        results = run_transactions(db, args.warehouses)
    finally:
        db.close()

    passed = sum(1 for result in results if result.status == "passed")
    errors = sum(1 for result in results if result.status == "error")
    payload = {
        "metadata": {
            "engine": args.engine,
            "suite": "tpc-c",
            "timestamp": datetime.now().isoformat(),
            "warehouses": args.warehouses,
            "duration": args.duration,
            "host": args.host,
            "benchmark_profile": "bounded_tpcc_transaction_profile",
            "certification_status": "not_certified_tpc_c_publication",
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
    output_file = args.output_dir / f"tpc-c-{args.engine}-{datetime.now():%Y%m%d-%H%M%S}.json"
    output_file.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    print(f"Results saved to: {output_file}")
    return 0 if errors == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
