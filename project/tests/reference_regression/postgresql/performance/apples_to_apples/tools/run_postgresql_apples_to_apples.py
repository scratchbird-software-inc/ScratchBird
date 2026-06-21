#!/usr/bin/env python3
"""Run the PostgreSQL apples-to-apples reference performance workload.

The runner intentionally writes benchmark outputs under build/. Source-tree
files define the workload only.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import platform
import re
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import date, datetime, timedelta, timezone
from pathlib import Path
from typing import Any


SUITE_ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = SUITE_ROOT.parents[5]
DEFAULT_OUTPUT_ROOT = (
    REPO_ROOT
    / "build"
    / "reference-regression"
    / "postgresql"
    / "performance"
    / "apples-to-apples"
)
IDENT_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


@dataclass(frozen=True)
class Scale:
    customers: int
    products: int
    orders: int
    order_items: int
    bulk_insert_rows: int


SCALES: dict[str, Scale] = {
    "tiny": Scale(
        customers=1000,
        products=500,
        orders=5000,
        order_items=20000,
        bulk_insert_rows=10000,
    ),
    "small": Scale(
        customers=10000,
        products=5000,
        orders=50000,
        order_items=200000,
        bulk_insert_rows=100000,
    ),
}


COUNTRIES = ["US", "CA", "MX", "GB", "DE", "FR", "IT", "ES", "JP", "AU"]
STATUSES = ["pending", "paid", "shipped", "delivered", "cancelled"]
CATEGORIES = [
    "hardware",
    "software",
    "books",
    "games",
    "office",
    "garden",
    "grocery",
    "sports",
]


TABLE_COLUMNS: dict[str, list[str]] = {
    "customers": [
        "customer_id",
        "first_name",
        "last_name",
        "email",
        "phone",
        "registration_date",
        "country_code",
        "account_balance",
    ],
    "products": [
        "product_id",
        "product_code",
        "name",
        "category",
        "price",
        "cost",
        "stock_quantity",
        "is_active",
    ],
    "orders": [
        "order_id",
        "customer_id",
        "order_date",
        "status",
        "total_amount",
        "shipping_cost",
        "discount_amount",
    ],
    "order_items": [
        "item_id",
        "order_id",
        "product_id",
        "quantity",
        "unit_price",
        "discount_pct",
    ],
}


def utc_timestamp() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def quote_ident(value: str) -> str:
    if not IDENT_RE.match(value):
        raise ValueError(f"Unsafe SQL identifier: {value!r}")
    return f'"{value}"'


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def sha256_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def run_command(command: list[str], *, env: dict[str, str], cwd: Path | None = None) -> dict[str, Any]:
    started = time.perf_counter()
    proc = subprocess.run(
        command,
        cwd=str(cwd) if cwd else None,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    duration_ms = (time.perf_counter() - started) * 1000.0
    return {
        "returncode": proc.returncode,
        "stdout": proc.stdout,
        "stderr": proc.stderr,
        "duration_ms": duration_ms,
        "command": command,
    }


class PgRunner:
    def __init__(self, args: argparse.Namespace, run_root: Path, scale: Scale):
        self.args = args
        self.run_root = run_root
        self.scale = scale
        self.schema_ident = quote_ident(args.schema)
        self.env = os.environ.copy()
        if args.password:
            self.env["PGPASSWORD"] = args.password

    def psql_base(self) -> list[str]:
        return [
            "psql",
            "-h",
            self.args.host,
            "-p",
            str(self.args.port),
            "-U",
            self.args.user,
            "-d",
            self.args.database,
            "-X",
            "-v",
            "ON_ERROR_STOP=1",
            "-v",
            f"schema_name={self.args.schema}",
            "-v",
            f"bulk_insert_rows={self.scale.bulk_insert_rows}",
            "-P",
            "pager=off",
            "-A",
            "-t",
        ]

    def env_with_search_path(self) -> dict[str, str]:
        env = self.env.copy()
        pgoptions = env.get("PGOPTIONS", "")
        search_path_option = f"-c search_path={self.args.schema}"
        env["PGOPTIONS"] = f"{pgoptions} {search_path_option}".strip()
        return env

    def run_sql_text(
        self,
        sql: str,
        *,
        output_path: Path | None = None,
        timing_only: bool = False,
        env: dict[str, str] | None = None,
    ) -> dict[str, Any]:
        command = self.psql_base() + ["-f", "-"]
        command_env = env if env is not None else self.env
        started = time.perf_counter()
        if output_path is None:
            proc = subprocess.run(
                command,
                input=sql,
                env=command_env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            stdout = proc.stdout
            stdout_bytes = len(stdout.encode("utf-8"))
            stdout_sha = sha256_text(stdout)
        else:
            output_path.parent.mkdir(parents=True, exist_ok=True)
            with output_path.open("w", encoding="utf-8") as out:
                proc = subprocess.run(
                    command,
                    input=sql,
                    env=command_env,
                    text=True,
                    stdout=out,
                    stderr=subprocess.PIPE,
                    check=False,
                )
            stdout = "" if timing_only else output_path.read_text(encoding="utf-8", errors="replace")
            stdout_bytes = output_path.stat().st_size
            stdout_sha = sha256_file(output_path)
        duration_ms = (time.perf_counter() - started) * 1000.0
        return {
            "returncode": proc.returncode,
            "stdout": stdout,
            "stderr": proc.stderr,
            "duration_ms": duration_ms,
            "stdout_bytes": stdout_bytes,
            "stdout_sha256": stdout_sha,
        }

    def run_in_schema(self, sql: str, *, output_path: Path | None = None) -> dict[str, Any]:
        return self.run_sql_text(
            f"{sql}\n",
            output_path=output_path,
            timing_only=output_path is not None,
            env=self.env_with_search_path(),
        )

    def scalar_json(self, sql: str) -> Any:
        result = self.run_sql_text(sql)
        if result["returncode"] != 0:
            return {
                "error": result["stderr"].strip(),
                "returncode": result["returncode"],
            }
        raw = result["stdout"].strip()
        if not raw:
            return None
        if "\n" in raw:
            raw = raw.splitlines()[-1].strip()
        try:
            return json.loads(raw)
        except json.JSONDecodeError:
            return raw


def write_csv(path: Path, header: list[str], rows: Any) -> dict[str, Any]:
    started = time.perf_counter()
    count = 0
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        for row in rows:
            writer.writerow(row)
            count += 1
    duration_ms = (time.perf_counter() - started) * 1000.0
    return {
        "path": str(path),
        "columns": header,
        "row_count": count,
        "bytes": path.stat().st_size,
        "sha256": sha256_file(path),
        "generation_duration_ms": duration_ms,
    }


def sql_literal(value: Any) -> str:
    if value is None:
        return "NULL"
    text = str(value)
    if text.upper() == "NULL":
        return "NULL"
    escaped = text.replace("'", "''")
    return f"'{escaped}'"


def write_insert_script(
    path: Path,
    *,
    physical_table: str,
    columns: list[str],
    rows: list[list[Any]],
    batch_size: int,
) -> dict[str, Any]:
    started = time.perf_counter()
    statement_count = 0
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as handle:
        for start in range(0, len(rows), batch_size):
            chunk = rows[start : start + batch_size]
            values = [
                "(" + ", ".join(sql_literal(value) for value in row) + ")"
                for row in chunk
            ]
            handle.write(
                f"INSERT INTO {physical_table} ({', '.join(columns)}) VALUES\n"
                + ",\n".join(values)
                + ";\n"
            )
            statement_count += 1
    duration_ms = (time.perf_counter() - started) * 1000.0
    return {
        "path": str(path),
        "table": physical_table,
        "row_count": len(rows),
        "statement_count": statement_count,
        "bytes": path.stat().st_size,
        "sha256": sha256_file(path),
        "generation_duration_ms": duration_ms,
        "batch_size": batch_size,
    }


def generate_insert_scripts(run_root: Path, scale: Scale, batch_size: int) -> dict[str, Any]:
    script_dir = run_root / "generated-load-sql"
    rows_by_table = {
        "customers": list(generate_customers(scale.customers)),
        "products": list(generate_products(scale.products)),
        "orders": list(generate_orders(scale.orders, scale.customers)),
        "order_items": list(generate_order_items(scale.order_items, scale.orders, scale.products)),
    }
    result = {}
    for table, rows in rows_by_table.items():
        result[table] = write_insert_script(
            script_dir / f"{table}.sql",
            physical_table=table,
            columns=TABLE_COLUMNS[table],
            rows=rows,
            batch_size=batch_size,
        )
    return result


def write_combined_insert_script(run_root: Path, insert_scripts: dict[str, Any]) -> dict[str, Any]:
    started = time.perf_counter()
    path = run_root / "generated-load-sql" / "combined_load.sql"
    path.parent.mkdir(parents=True, exist_ok=True)
    total_rows = 0
    total_statements = 0
    tables: list[dict[str, Any]] = []
    with path.open("w", encoding="utf-8") as handle:
        for table, meta in insert_scripts.items():
            source_path = Path(meta["path"])
            handle.write(f"-- combined-load-table: {table}\n")
            handle.write(source_path.read_text(encoding="utf-8"))
            handle.write("\n")
            total_rows += int(meta["row_count"])
            total_statements += int(meta["statement_count"])
            tables.append(
                {
                    "table_name": table,
                    "row_count": meta["row_count"],
                    "statement_count": meta["statement_count"],
                    "source_path": meta["path"],
                    "source_sha256": meta["sha256"],
                }
            )
    duration_ms = (time.perf_counter() - started) * 1000.0
    return {
        "path": str(path),
        "table": "__combined__",
        "row_count": total_rows,
        "statement_count": total_statements,
        "tables": tables,
        "bytes": path.stat().st_size,
        "sha256": sha256_file(path),
        "generation_duration_ms": duration_ms,
    }


def generate_customers(row_count: int):
    start = date(2020, 1, 1)
    for i in range(1, row_count + 1):
        yield [
            i,
            f"First{i % 1000}",
            f"Last{i % 2000}",
            f"user{i}@example.invalid",
            f"+1-555-{i % 10000:04d}",
            (start + timedelta(days=i % 1826)).isoformat(),
            COUNTRIES[i % len(COUNTRIES)],
            f"{((i * 197) % 2000000) / 100:.2f}",
        ]


def generate_products(row_count: int):
    for i in range(1, row_count + 1):
        price = ((i * 131) % 100000) / 100 + 1
        cost = price * 0.62
        yield [
            i,
            f"SKU{i:09d}",
            f"Product {i}",
            CATEGORIES[i % len(CATEGORIES)],
            f"{price:.2f}",
            f"{cost:.2f}",
            (i * 17) % 10000,
            0 if i % 23 == 0 else 1,
        ]


def generate_orders(row_count: int, customer_count: int):
    start = datetime(2021, 1, 1, 0, 0, 0)
    for i in range(1, row_count + 1):
        total = ((i * 173) % 250000) / 100 + 10
        yield [
            i,
            ((i * 37) % customer_count) + 1,
            (start + timedelta(hours=i % 35064)).strftime("%Y-%m-%d %H:%M:%S"),
            STATUSES[i % len(STATUSES)],
            f"{total:.2f}",
            f"{((i * 19) % 3500) / 100:.2f}",
            f"{((i * 11) % 2000) / 100:.2f}",
        ]


def generate_order_items(row_count: int, order_count: int, product_count: int):
    for i in range(1, row_count + 1):
        yield [
            i,
            ((i * 17) % order_count) + 1,
            ((i * 13) % product_count) + 1,
            (i % 64) + 1,
            f"{(((i * 131) % 100000) / 100 + 1):.2f}",
            f"{0 if i % 5 else ((i % 20) / 2):.2f}",
        ]


def collect_database_stats(pg: PgRunner) -> Any:
    return pg.scalar_json(
        "SELECT to_jsonb(s) FROM pg_stat_database s WHERE datname = current_database();"
    )


def numeric_delta(before: Any, after: Any) -> dict[str, float]:
    if not isinstance(before, dict) or not isinstance(after, dict):
        return {}
    deltas: dict[str, float] = {}
    for key, after_value in after.items():
        before_value = before.get(key)
        if isinstance(before_value, (int, float)) and isinstance(after_value, (int, float)):
            deltas[key] = after_value - before_value
    return deltas


def collect_system_snapshot(pg: PgRunner) -> dict[str, Any]:
    settings = [
        "server_version",
        "data_directory",
        "shared_buffers",
        "work_mem",
        "maintenance_work_mem",
        "effective_cache_size",
        "synchronous_commit",
        "fsync",
        "full_page_writes",
        "wal_level",
        "max_wal_size",
        "checkpoint_timeout",
        "max_worker_processes",
        "max_parallel_workers_per_gather",
        "jit",
        "random_page_cost",
        "seq_page_cost",
    ]
    settings_sql = (
        "SELECT jsonb_object_agg(name, setting) "
        "FROM pg_settings WHERE name = ANY(ARRAY["
        + ",".join("'" + item + "'" for item in settings)
        + "]);"
    )
    return {
        "platform": {
            "system": platform.system(),
            "release": platform.release(),
            "machine": platform.machine(),
            "processor": platform.processor(),
            "python": platform.python_version(),
        },
        "postgresql_version": pg.scalar_json("SELECT to_json(version());"),
        "settings": pg.scalar_json(settings_sql),
        "database_stats": collect_database_stats(pg),
        "bgwriter_stats": pg.scalar_json("SELECT to_jsonb(pg_stat_bgwriter) FROM pg_stat_bgwriter;"),
        "wal_stats": pg.scalar_json("SELECT to_jsonb(pg_stat_wal) FROM pg_stat_wal;"),
        "io_stats": pg.scalar_json(
            """
            SELECT COALESCE(jsonb_agg(to_jsonb(x) ORDER BY backend_type, object, context), '[]'::jsonb)
            FROM (
                SELECT backend_type, object, context,
                       SUM(reads) AS reads,
                       SUM(writes) AS writes,
                       SUM(writebacks) AS writebacks,
                       SUM(extends) AS extends,
                       SUM(hits) AS hits,
                       SUM(evictions) AS evictions,
                       SUM(fsyncs) AS fsyncs,
                       MAX(op_bytes) AS op_bytes
                FROM pg_stat_io
                GROUP BY backend_type, object, context
            ) x;
            """
        ),
    }


def collect_relation_metrics(pg: PgRunner) -> Any:
    return pg.scalar_json(
        f"""
        SELECT COALESCE(jsonb_agg(to_jsonb(x) ORDER BY x.relation_name), '[]'::jsonb)
        FROM (
            SELECT c.relname AS relation_name,
                   c.relkind AS relation_kind,
                   pg_relation_size(c.oid) AS relation_bytes,
                   pg_total_relation_size(c.oid) AS total_bytes,
                   COALESCE(s.n_live_tup, 0) AS estimated_live_tuples,
                   COALESCE(s.seq_scan, 0) AS seq_scan,
                   COALESCE(s.idx_scan, 0) AS idx_scan,
                   COALESCE(s.n_tup_ins, 0) AS tuples_inserted,
                   COALESCE(s.n_tup_upd, 0) AS tuples_updated,
                   COALESCE(s.n_tup_del, 0) AS tuples_deleted
            FROM pg_class c
            JOIN pg_namespace n ON n.oid = c.relnamespace
            LEFT JOIN pg_stat_user_tables s ON s.relid = c.oid
            WHERE n.nspname = '{pg.args.schema}'
              AND c.relkind IN ('r', 'i')
        ) x;
        """
    )


def verify_data(pg: PgRunner, scale: Scale) -> dict[str, Any]:
    expected = {
        "customers": scale.customers,
        "products": scale.products,
        "orders": scale.orders,
        "order_items": scale.order_items,
    }
    counts = pg.scalar_json(
        f"""
        SELECT jsonb_build_object(
            'customers', (SELECT COUNT(*) FROM {pg.schema_ident}.customers),
            'products', (SELECT COUNT(*) FROM {pg.schema_ident}.products),
            'orders', (SELECT COUNT(*) FROM {pg.schema_ident}.orders),
            'order_items', (SELECT COUNT(*) FROM {pg.schema_ident}.order_items),
            'invalid_order_customers', (
                SELECT COUNT(*)
                FROM {pg.schema_ident}.orders o
                LEFT JOIN {pg.schema_ident}.customers c ON o.customer_id = c.customer_id
                WHERE c.customer_id IS NULL
            ),
            'invalid_item_orders', (
                SELECT COUNT(*)
                FROM {pg.schema_ident}.order_items oi
                LEFT JOIN {pg.schema_ident}.orders o ON oi.order_id = o.order_id
                WHERE o.order_id IS NULL
            ),
            'invalid_item_products', (
                SELECT COUNT(*)
                FROM {pg.schema_ident}.order_items oi
                LEFT JOIN {pg.schema_ident}.products p ON oi.product_id = p.product_id
                WHERE p.product_id IS NULL
            )
        );
        """
    )
    passed = isinstance(counts, dict) and all(counts.get(k) == v for k, v in expected.items())
    passed = passed and counts.get("invalid_order_customers") == 0
    passed = passed and counts.get("invalid_item_orders") == 0
    passed = passed and counts.get("invalid_item_products") == 0
    return {
        "expected": expected,
        "actual": counts,
        "passed": passed,
    }


def parse_command_count(stdout: str) -> int | None:
    for line in reversed([item.strip() for item in stdout.splitlines() if item.strip()]):
        parts = line.split()
        if not parts:
            continue
        if parts[0] in {"INSERT", "UPDATE", "DELETE", "COPY"}:
            try:
                return int(parts[-1])
            except ValueError:
                return None
    return None


def command_counts(stdout: str) -> list[int]:
    counts: list[int] = []
    for line in [item.strip() for item in stdout.splitlines() if item.strip()]:
        parts = line.split()
        if not parts or parts[0] not in {"INSERT", "UPDATE", "DELETE", "COPY"}:
            continue
        try:
            counts.append(int(parts[-1]))
        except ValueError:
            continue
    return counts


def count_output_rows(path: Path) -> int:
    with path.open("rb") as handle:
        return sum(1 for line in handle if line.strip())


def run_setup(pg: PgRunner) -> dict[str, Any]:
    setup_file = SUITE_ROOT / "sql" / "setup" / "000_schema.sql"
    sql = setup_file.read_text(encoding="utf-8")
    result = pg.run_sql_text(sql)
    return {
        "path": str(setup_file),
        "duration_ms": result["duration_ms"],
        "status": "passed" if result["returncode"] == 0 else "failed",
        "stderr": result["stderr"],
    }


def run_index_setup(pg: PgRunner) -> dict[str, Any]:
    index_file = SUITE_ROOT / "sql" / "setup" / "010_indexes_and_statistics.sql"
    result = pg.run_in_schema(index_file.read_text(encoding="utf-8"))
    return {
        "path": str(index_file),
        "duration_ms": result["duration_ms"],
        "status": "passed" if result["returncode"] == 0 else "failed",
        "stderr": result["stderr"],
    }


def generate_data_files(run_root: Path, scale: Scale) -> dict[str, Any]:
    data_dir = run_root / "data"
    tables = {
        "customers": (
            [
                "customer_id",
                "first_name",
                "last_name",
                "email",
                "phone",
                "registration_date",
                "country_code",
                "account_balance",
            ],
            generate_customers(scale.customers),
        ),
        "products": (
            [
                "product_id",
                "product_code",
                "name",
                "category",
                "price",
                "cost",
                "stock_quantity",
                "is_active",
            ],
            generate_products(scale.products),
        ),
        "orders": (
            [
                "order_id",
                "customer_id",
                "order_date",
                "status",
                "total_amount",
                "shipping_cost",
                "discount_amount",
            ],
            generate_orders(scale.orders, scale.customers),
        ),
        "order_items": (
            [
                "item_id",
                "order_id",
                "product_id",
                "quantity",
                "unit_price",
                "discount_pct",
            ],
            generate_order_items(scale.order_items, scale.orders, scale.products),
        ),
    }
    result = {}
    for table, (columns, rows) in tables.items():
        result[table] = write_csv(data_dir / f"{table}.csv", columns, rows)
    return result


def copy_data(pg: PgRunner, data_files: dict[str, Any]) -> list[dict[str, Any]]:
    results: list[dict[str, Any]] = []
    for table, meta in data_files.items():
        columns = ", ".join(meta["columns"])
        path = Path(meta["path"])
        sql = (
            f"\\copy {table} ({columns}) FROM '{path}' WITH (FORMAT csv)\n"
        )
        copy_result = pg.run_sql_text(sql, env=pg.env_with_search_path())
        rows = parse_command_count(copy_result["stdout"]) or 0
        duration_s = copy_result["duration_ms"] / 1000.0
        results.append(
            {
                "table_name": table,
                "rows": rows,
                "expected_rows": meta["row_count"],
                "csv_bytes": meta["bytes"],
                "csv_sha256": meta["sha256"],
                "duration_ms": copy_result["duration_ms"],
                "rows_per_second": rows / duration_s if duration_s else 0.0,
                "bytes_per_second": meta["bytes"] / duration_s if duration_s else 0.0,
                "status": "passed" if copy_result["returncode"] == 0 and rows == meta["row_count"] else "failed",
                "stdout": copy_result["stdout"].strip(),
                "stderr": copy_result["stderr"],
                "statement_count": 1,
                "load_execution_mode": "copy",
            }
        )
    return results


def run_insert_load(pg: PgRunner, insert_scripts: dict[str, Any]) -> list[dict[str, Any]]:
    results = []
    for table, meta in insert_scripts.items():
        sql = Path(meta["path"]).read_text(encoding="utf-8")
        result = pg.run_sql_text(sql, env=pg.env_with_search_path())
        counts = command_counts(result["stdout"])
        rows = sum(counts)
        duration_s = result["duration_ms"] / 1000.0
        results.append(
            {
                "table_name": table,
                "rows": rows,
                "expected_rows": meta["row_count"],
                "script_path": meta["path"],
                "script_bytes": meta["bytes"],
                "script_sha256": meta["sha256"],
                "duration_ms": result["duration_ms"],
                "rows_per_second": rows / duration_s if duration_s else 0.0,
                "status": "passed" if result["returncode"] == 0 and rows == meta["row_count"] else "failed",
                "stdout": result["stdout"].strip(),
                "stderr": result["stderr"],
                "statement_count": meta["statement_count"],
                "command_counts": counts,
                "load_execution_mode": "insert-per-table",
            }
        )
    return results


def run_combined_insert_load(pg: PgRunner, insert_scripts: dict[str, Any]) -> list[dict[str, Any]]:
    combined_meta = write_combined_insert_script(pg.run_root, insert_scripts)
    sql = Path(combined_meta["path"]).read_text(encoding="utf-8")
    result = pg.run_sql_text(sql, env=pg.env_with_search_path())
    counts = command_counts(result["stdout"])
    rows = sum(counts)
    duration_s = result["duration_ms"] / 1000.0
    return [
        {
            "table_name": "__combined__",
            "rows": rows,
            "expected_rows": combined_meta["row_count"],
            "script_path": combined_meta["path"],
            "script_bytes": combined_meta["bytes"],
            "script_sha256": combined_meta["sha256"],
            "duration_ms": result["duration_ms"],
            "rows_per_second": rows / duration_s if duration_s else 0.0,
            "status": "passed" if result["returncode"] == 0 and rows == combined_meta["row_count"] else "failed",
            "stdout": result["stdout"].strip(),
            "stderr": result["stderr"],
            "statement_count": combined_meta["statement_count"],
            "command_counts": counts,
            "load_execution_mode": "insert-combined",
        }
    ]


def action_sql(action: dict[str, Any]) -> tuple[Path, str]:
    path = SUITE_ROOT / action["path"]
    return path, path.read_text(encoding="utf-8")


def capture_plan_only(pg: PgRunner, action: dict[str, Any], sql: str, plan_dir: Path) -> dict[str, Any]:
    plan_dir.mkdir(parents=True, exist_ok=True)
    plan_path = plan_dir / f"{action['id']}.plan.json"
    explain_sql = f"EXPLAIN (FORMAT JSON) {sql.rstrip().rstrip(';')};"
    result = pg.run_in_schema(explain_sql, output_path=plan_path)
    return {
        "path": str(plan_path),
        "status": "passed" if result["returncode"] == 0 else "failed",
        "duration_ms": result["duration_ms"],
        "stderr": result["stderr"],
        "bytes": result["stdout_bytes"],
        "sha256": result["stdout_sha256"],
    }


def capture_analyze_plan(pg: PgRunner, action: dict[str, Any], sql: str, explain_dir: Path) -> dict[str, Any]:
    explain_dir.mkdir(parents=True, exist_ok=True)
    explain_path = explain_dir / f"{action['id']}.explain_analyze.json"
    explain_sql = f"EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON) {sql.rstrip().rstrip(';')};"
    result = pg.run_in_schema(explain_sql, output_path=explain_path)
    summary: dict[str, Any] = {}
    if result["returncode"] == 0:
        try:
            payload = json.loads(explain_path.read_text(encoding="utf-8"))
            root = payload[0] if payload else {}
            summary = {
                "planning_time_ms": root.get("Planning Time"),
                "execution_time_ms": root.get("Execution Time"),
                "top_node": root.get("Plan", {}).get("Node Type") if isinstance(root.get("Plan"), dict) else None,
            }
        except Exception as exc:  # pragma: no cover - diagnostic artifact best effort
            summary = {"parse_error": str(exc)}
    return {
        "path": str(explain_path),
        "status": "passed" if result["returncode"] == 0 else "failed",
        "duration_ms": result["duration_ms"],
        "stderr": result["stderr"],
        "bytes": result["stdout_bytes"],
        "sha256": result["stdout_sha256"],
        "summary": summary,
    }


def run_actions(pg: PgRunner, manifest: dict[str, Any]) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    output_dir = pg.run_root / "action-output"
    plan_dir = pg.run_root / "plans"
    results: list[dict[str, Any]] = []
    plan_only: list[dict[str, Any]] = []

    for action in manifest["actions"]:
        path, sql = action_sql(action)
        if action["kind"] == "dml":
            plan_only.append(
                {
                    "action_id": action["id"],
                    **capture_plan_only(pg, action, sql, plan_dir),
                }
            )
        output_path = output_dir / f"{action['id']}.out"
        db_stats_before = collect_database_stats(pg)
        relation_metrics_before = collect_relation_metrics(pg) if action["kind"] == "dml" else None
        result = pg.run_in_schema(sql, output_path=output_path)
        db_stats_after = collect_database_stats(pg)
        relation_metrics_after = collect_relation_metrics(pg) if action["kind"] == "dml" else None
        duration_s = result["duration_ms"] / 1000.0
        if action["kind"] == "query":
            rows = count_output_rows(output_path)
            rows_per_second = rows / duration_s if duration_s else 0.0
            rows_affected = None
        else:
            rows_affected = parse_command_count(output_path.read_text(encoding="utf-8", errors="replace"))
            rows = None
            rows_per_second = (rows_affected or 0) / duration_s if duration_s else 0.0

        results.append(
            {
                "action_id": action["id"],
                "kind": action["kind"],
                "path": str(path),
                "sql_sha256": sha256_file(path),
                "status": "passed" if result["returncode"] == 0 else "failed",
                "duration_ms": result["duration_ms"],
                "rows_returned": rows,
                "rows_affected": rows_affected,
                "rows_per_second": rows_per_second,
                "stdout_path": str(output_path),
                "stdout_bytes": result["stdout_bytes"],
                "stdout_sha256": result["stdout_sha256"],
                "stderr": result["stderr"],
                "database_stats_before": db_stats_before,
                "database_stats_after": db_stats_after,
                "database_stats_delta": numeric_delta(db_stats_before, db_stats_after),
                "relation_metrics_before": relation_metrics_before,
                "relation_metrics_after": relation_metrics_after,
            }
        )
    return results, plan_only


def capture_query_explains(pg: PgRunner, manifest: dict[str, Any]) -> list[dict[str, Any]]:
    explains: list[dict[str, Any]] = []
    explain_dir = pg.run_root / "explain-analyze"
    for action in manifest["actions"]:
        if action["kind"] != "query":
            continue
        _, sql = action_sql(action)
        explains.append(
            {
                "action_id": action["id"],
                **capture_analyze_plan(pg, action, sql, explain_dir),
            }
        )
    return explains


def write_csv_summary(run_root: Path, actions: list[dict[str, Any]], load: list[dict[str, Any]]) -> None:
    with (run_root / "action-summary.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=[
                "action_id",
                "kind",
                "status",
                "duration_ms",
                "rows_returned",
                "rows_affected",
                "rows_per_second",
                "stdout_bytes",
            ],
        )
        writer.writeheader()
        for row in actions:
            writer.writerow({key: row.get(key) for key in writer.fieldnames})

    with (run_root / "load-summary.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=[
                "table_name",
                "status",
                "rows",
                "expected_rows",
                "csv_bytes",
                "script_bytes",
                "duration_ms",
                "rows_per_second",
                "bytes_per_second",
                "statement_count",
                "load_execution_mode",
            ],
        )
        writer.writeheader()
        for row in load:
            writer.writerow({key: row.get(key) for key in writer.fieldnames})


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5432)
    parser.add_argument("--database", default="sbbench")
    parser.add_argument("--user", default="sbbench")
    parser.add_argument("--password", default=os.getenv("PGPASSWORD", ""))
    parser.add_argument("--schema", default="sb_pg_reference_bench")
    parser.add_argument("--scale", choices=sorted(SCALES), default="small")
    parser.add_argument("--insert-batch-size", type=int, default=500)
    parser.add_argument(
        "--load-execution-mode",
        choices=("copy", "insert-per-table", "insert-combined"),
        default="copy",
        help="Run PostgreSQL load as protocol COPY or generated INSERT statements.",
    )
    parser.add_argument("--output-root", type=Path, default=DEFAULT_OUTPUT_ROOT)
    parser.add_argument("--skip-explain-analyze", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    scale = SCALES[args.scale]
    run_root = args.output_root / f"postgresql-{args.scale}-{utc_timestamp()}"
    run_root.mkdir(parents=True, exist_ok=False)
    pg = PgRunner(args, run_root, scale)

    manifest = json.loads((SUITE_ROOT / "manifest.json").read_text(encoding="utf-8"))
    started = time.perf_counter()

    psql_version = run_command(["psql", "--version"], env=pg.env)
    provenance = {
        "schema_version": "scratchbird.reference_regression.postgresql.apples_to_apples.result.v1",
        "timestamp_utc": utc_timestamp(),
        "suite_root": str(SUITE_ROOT),
        "run_root": str(run_root),
        "scale": args.scale,
        "load_execution_mode": args.load_execution_mode,
        "insert_batch_size": args.insert_batch_size,
        "scale_counts": scale.__dict__,
        "connection": {
            "host": args.host,
            "port": args.port,
            "database": args.database,
            "user": args.user,
            "schema": args.schema,
        },
        "psql_version": psql_version["stdout"].strip(),
        "manifest_sha256": sha256_file(SUITE_ROOT / "manifest.json"),
    }

    pre_snapshot = collect_system_snapshot(pg)
    setup = run_setup(pg)
    if setup["status"] != "passed":
        payload = {"provenance": provenance, "setup": setup, "status": "failed"}
        (run_root / "summary.json").write_text(json.dumps(payload, indent=2), encoding="utf-8")
        return 1

    data_files = generate_data_files(run_root, scale)
    insert_scripts: dict[str, Any] = {}
    if args.load_execution_mode == "copy":
        load_results = copy_data(pg, data_files)
    else:
        insert_scripts = generate_insert_scripts(run_root, scale, args.insert_batch_size)
        if args.load_execution_mode == "insert-combined":
            load_results = run_combined_insert_load(pg, insert_scripts)
        else:
            load_results = run_insert_load(pg, insert_scripts)
    index_setup = run_index_setup(pg)
    verification = verify_data(pg, scale)
    relation_after_load = collect_relation_metrics(pg)
    action_results, dml_plan_only = run_actions(pg, manifest)
    relation_after_actions = collect_relation_metrics(pg)
    query_explains = [] if args.skip_explain_analyze else capture_query_explains(pg, manifest)
    post_snapshot = collect_system_snapshot(pg)

    elapsed_ms = (time.perf_counter() - started) * 1000.0
    status = "passed"
    if any(item["status"] != "passed" for item in load_results):
        status = "failed"
    if index_setup["status"] != "passed" or not verification["passed"]:
        status = "failed"
    if any(item["status"] != "passed" for item in action_results):
        status = "failed"

    total_load_rows = sum(item["rows"] for item in load_results)
    total_load_ms = sum(item["duration_ms"] for item in load_results)
    summary = {
        "provenance": provenance,
        "status": status,
        "elapsed_ms": elapsed_ms,
        "setup": setup,
        "data_files": data_files,
        "insert_scripts": insert_scripts,
        "load_results": load_results,
        "load_summary": {
            "load_execution_mode": args.load_execution_mode,
            "rows": total_load_rows,
            "duration_ms": total_load_ms,
            "rows_per_second": total_load_rows / (total_load_ms / 1000.0) if total_load_ms else 0.0,
            "statement_count": sum(int(item.get("statement_count") or 0) for item in load_results),
        },
        "index_setup": index_setup,
        "verification": verification,
        "actions": action_results,
        "dml_plan_only": dml_plan_only,
        "query_explain_analyze": query_explains,
        "pre_snapshot": pre_snapshot,
        "post_snapshot": post_snapshot,
        "relation_metrics_after_load": relation_after_load,
        "relation_metrics_after_actions": relation_after_actions,
    }
    (run_root / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    write_csv_summary(run_root, action_results, load_results)
    print(json.dumps({"status": status, "run_root": str(run_root), "summary": str(run_root / "summary.json")}, indent=2))
    return 0 if status == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
