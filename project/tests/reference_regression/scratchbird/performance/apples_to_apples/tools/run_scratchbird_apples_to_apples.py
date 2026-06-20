#!/usr/bin/env python3
"""Run the ScratchBird apples-to-apples performance workload.

The runner intentionally writes benchmark outputs under build/. Source-tree
files define only the workload and runner.
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
from typing import Any, Iterable


SUITE_ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = SUITE_ROOT.parents[5]
DEFAULT_OUTPUT_ROOT = (
    REPO_ROOT
    / "build"
    / "reference-regression"
    / "scratchbird"
    / "performance"
    / "apples-to-apples"
)
DEFAULT_LATEST_JSON = Path(
    "/home/dcalford/CliWork/local_work/scratchbird-driver-test-server/latest.json"
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

TEXT_COLUMNS = {
    "first_name",
    "last_name",
    "email",
    "phone",
    "registration_date",
    "country_code",
    "product_code",
    "name",
    "category",
    "order_date",
    "status",
    "data",
}


def utc_timestamp() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def validate_ident(value: str) -> str:
    if not IDENT_RE.match(value):
        raise ValueError(f"Unsafe SQL identifier: {value!r}")
    return value


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def sha256_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def shell_literal(value: Any, column: str) -> str:
    if value is None:
        return "NULL"
    if column in TEXT_COLUMNS:
        text = str(value).replace("'", "''")
        return f"'{text}'"
    return str(value)


def count_output_rows(path: Path) -> int:
    with path.open("rb") as handle:
        return sum(1 for line in handle if line.strip())


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


def table_generators(scale: Scale):
    return {
        "customers": generate_customers(scale.customers),
        "products": generate_products(scale.products),
        "orders": generate_orders(scale.orders, scale.customers),
        "order_items": generate_order_items(scale.order_items, scale.orders, scale.products),
    }


def process_snapshot(pid: int | None) -> dict[str, Any] | None:
    if not pid:
        return None
    proc_dir = Path("/proc") / str(pid)
    stat_path = proc_dir / "stat"
    status_path = proc_dir / "status"
    if not stat_path.exists():
        return {"pid": pid, "available": False}
    result: dict[str, Any] = {"pid": pid, "available": True}
    try:
        parts = stat_path.read_text(encoding="utf-8", errors="replace").split()
        result.update(
            {
                "state": parts[2],
                "utime_ticks": int(parts[13]),
                "stime_ticks": int(parts[14]),
                "vsize_bytes": int(parts[22]),
                "rss_pages": int(parts[23]),
            }
        )
    except Exception as exc:
        result["stat_error"] = str(exc)
    try:
        status = status_path.read_text(encoding="utf-8", errors="replace")
        for key in ("VmRSS", "VmSize", "Threads", "voluntary_ctxt_switches", "nonvoluntary_ctxt_switches"):
            match = re.search(rf"^{re.escape(key)}:\s+(.+)$", status, re.MULTILINE)
            if match:
                result[key] = match.group(1).strip()
    except Exception as exc:
        result["status_error"] = str(exc)
    return result


class SbRunner:
    def __init__(self, args: argparse.Namespace, run_root: Path, scale: Scale, latest: dict[str, Any]):
        self.args = args
        self.run_root = run_root
        self.scale = scale
        self.latest = latest
        self.schema = validate_ident(args.schema)
        self.full_schema = f"users.public.{self.schema}"
        self.sbsql = Path(args.sbsql or latest.get("sbsql") or "build/output/linux/bin/SBsql")
        self.database = args.database or latest.get("database")
        self.host = args.host or latest.get("tcp_host", "127.0.0.1")
        self.port = int(args.port or latest.get("tcp_port", 3092))
        self.sslmode = args.sslmode or latest.get("sslmode", "require")
        self.user = args.user or latest.get("user", "alice")
        self.password = args.password or latest.get("password_argument", "scratchbird")
        self.role = args.role or latest.get("user_role", "sysarch")
        self.table_prefix = args.table_prefix

    def table(self, logical_name: str) -> str:
        return validate_ident(f"{self.table_prefix}{logical_name}")

    def full_table(self, logical_name: str) -> str:
        return f"{self.full_schema}.{self.table(logical_name)}"

    def sbsql_base(self) -> list[str]:
        return [
            str(self.sbsql),
            str(self.database),
            f"--host={self.host}",
            f"--port={self.port}",
            f"--sslmode={self.sslmode}",
            "--conn-opt",
            f"role={self.role}",
            "-U",
            self.user,
            "-P",
            self.password,
            "-q",
            "-A",
            "-t",
            "-b",
        ]

    def run_sql_text(self, sql: str, *, output_path: Path | None = None) -> dict[str, Any]:
        command = self.sbsql_base() + ["-c", sql]
        return self._run(command, output_path=output_path)

    def run_file(self, path: Path, *, output_path: Path | None = None) -> dict[str, Any]:
        command = self.sbsql_base() + ["-f", str(path)]
        return self._run(command, output_path=output_path)

    def _run(self, command: list[str], *, output_path: Path | None = None) -> dict[str, Any]:
        started = time.perf_counter()
        if output_path is None:
            proc = subprocess.run(
                command,
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
                    text=True,
                    stdout=out,
                    stderr=subprocess.PIPE,
                    check=False,
                )
            stdout = ""
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
            "command": command,
        }

    def scalar(self, sql: str) -> Any:
        result = self.run_sql_text(sql)
        if result["returncode"] != 0:
            return {
                "returncode": result["returncode"],
                "stderr": result["stderr"].strip(),
                "stdout": result["stdout"].strip(),
            }
        raw = result["stdout"].strip()
        if not raw:
            return None
        if "\n" in raw:
            raw = raw.splitlines()[-1].strip()
        if re.fullmatch(r"-?\d+", raw):
            return int(raw)
        if re.fullmatch(r"-?\d+\.\d+", raw):
            return float(raw)
        return raw

    def process_snapshots(self) -> dict[str, Any]:
        return {
            "server": process_snapshot(self.latest.get("server_pid")),
            "listener": process_snapshot(self.latest.get("listener_pid")),
            "supervisor": process_snapshot(self.latest.get("supervisor_pid")),
        }


def write_csv_file(path: Path, columns: list[str], rows: Iterable[list[Any]]) -> dict[str, Any]:
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
        "columns": columns,
        "row_count": count,
        "bytes": path.stat().st_size,
        "sha256": sha256_file(path),
        "generation_duration_ms": duration_ms,
    }


def generate_data_files(run_root: Path, scale: Scale) -> dict[str, Any]:
    data_dir = run_root / "data"
    result = {}
    generators = table_generators(scale)
    for table, rows in generators.items():
        result[table] = write_csv_file(data_dir / f"{table}.csv", TABLE_COLUMNS[table], rows)
    return result


def batched(items: list[list[Any]], size: int):
    for offset in range(0, len(items), size):
        yield items[offset : offset + size]


def write_insert_script(
    path: Path,
    *,
    schema: str,
    physical_table: str,
    columns: list[str],
    rows: list[list[Any]],
    batch_size: int,
) -> dict[str, Any]:
    started = time.perf_counter()
    path.parent.mkdir(parents=True, exist_ok=True)
    statement_count = 0
    with path.open("w", encoding="utf-8") as handle:
        for batch in batched(rows, batch_size):
            values = []
            for row in batch:
                values.append(
                    "("
                    + ", ".join(shell_literal(value, column) for value, column in zip(row, columns))
                    + ")"
                )
            handle.write(
                f"INSERT INTO {schema}.{physical_table} ({', '.join(columns)}) VALUES\n"
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


def generate_insert_scripts(run_root: Path, sb: SbRunner, scale: Scale, batch_size: int) -> dict[str, Any]:
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
            script_dir / f"{table}.sbsql",
            schema=sb.full_schema,
            physical_table=sb.table(table),
            columns=TABLE_COLUMNS[table],
            rows=rows,
            batch_size=batch_size,
        )
    return result


def setup_statements(sb: SbRunner) -> list[tuple[str, str]]:
    s = sb.full_schema
    customers = sb.full_table("customers")
    products = sb.full_table("products")
    orders = sb.full_table("orders")
    order_items = sb.full_table("order_items")
    bulk_insert_test = sb.full_table("bulk_insert_test")
    return [
        ("create_schema", f"CREATE SCHEMA {s}"),
        (
            "create_customers",
            f"""
CREATE TABLE {customers} (
    customer_id BIGINT PRIMARY KEY,
    first_name VARCHAR(50),
    last_name VARCHAR(50),
    email VARCHAR(100) UNIQUE,
    phone VARCHAR(20),
    registration_date DATE,
    country_code VARCHAR(2),
    account_balance NUMERIC(12, 2)
)""",
        ),
        (
            "create_products",
            f"""
CREATE TABLE {products} (
    product_id BIGINT PRIMARY KEY,
    product_code VARCHAR(20) UNIQUE,
    name VARCHAR(200),
    category VARCHAR(50),
    price NUMERIC(10, 2),
    cost NUMERIC(10, 2),
    stock_quantity INTEGER,
    is_active INTEGER
)""",
        ),
        (
            "create_orders",
            f"""
CREATE TABLE {orders} (
    order_id BIGINT PRIMARY KEY,
    customer_id BIGINT,
    order_date TIMESTAMP,
    status VARCHAR(20),
    total_amount NUMERIC(12, 2),
    shipping_cost NUMERIC(8, 2),
    discount_amount NUMERIC(10, 2)
)""",
        ),
        (
            "create_order_items",
            f"""
CREATE TABLE {order_items} (
    item_id BIGINT PRIMARY KEY,
    order_id BIGINT,
    product_id BIGINT,
    quantity INTEGER,
    unit_price NUMERIC(10, 2),
    discount_pct NUMERIC(5, 2)
)""",
        ),
        (
            "create_bulk_insert_test",
            f"""
CREATE TABLE {bulk_insert_test} (
    id BIGINT PRIMARY KEY,
    data VARCHAR(100),
    metric_value NUMERIC(10, 2)
)""",
        ),
    ]


def run_setup(sb: SbRunner) -> list[dict[str, Any]]:
    results = []
    for name, sql in setup_statements(sb):
        result = sb.run_sql_text(sql)
        results.append(
            {
                "name": name,
                "sql_sha256": sha256_text(sql),
                "status": "passed" if result["returncode"] == 0 else "failed",
                "duration_ms": result["duration_ms"],
                "stdout": result["stdout"].strip(),
                "stderr": result["stderr"].strip(),
            }
        )
    return results


def index_statements(sb: SbRunner) -> list[tuple[str, str]]:
    return [
        ("idx_stress_customers_country_customer", f"CREATE INDEX idx_stress_customers_country_customer ON {sb.full_table('customers')} (country_code, customer_id)"),
        ("idx_stress_customers_registration", f"CREATE INDEX idx_stress_customers_registration ON {sb.full_table('customers')} (registration_date)"),
        ("idx_stress_customers_balance", f"CREATE INDEX idx_stress_customers_balance ON {sb.full_table('customers')} (account_balance)"),
        ("idx_stress_orders_customer_date", f"CREATE INDEX idx_stress_orders_customer_date ON {sb.full_table('orders')} (customer_id, order_date)"),
        ("idx_stress_orders_order_date", f"CREATE INDEX idx_stress_orders_order_date ON {sb.full_table('orders')} (order_date)"),
        ("idx_stress_orders_total_amount", f"CREATE INDEX idx_stress_orders_total_amount ON {sb.full_table('orders')} (total_amount)"),
        ("idx_stress_order_items_order_id", f"CREATE INDEX idx_stress_order_items_order_id ON {sb.full_table('order_items')} (order_id)"),
        ("idx_stress_order_items_product_id", f"CREATE INDEX idx_stress_order_items_product_id ON {sb.full_table('order_items')} (product_id)"),
        ("idx_stress_products_category", f"CREATE INDEX idx_stress_products_category ON {sb.full_table('products')} (category)"),
        ("analyze_customers", f"ANALYZE {sb.full_table('customers')}"),
        ("analyze_products", f"ANALYZE {sb.full_table('products')}"),
        ("analyze_orders", f"ANALYZE {sb.full_table('orders')}"),
        ("analyze_order_items", f"ANALYZE {sb.full_table('order_items')}"),
        ("analyze_bulk_insert_test", f"ANALYZE {sb.full_table('bulk_insert_test')}"),
    ]


def run_index_setup(sb: SbRunner) -> list[dict[str, Any]]:
    results = []
    for name, sql in index_statements(sb):
        result = sb.run_sql_text(sql)
        results.append(
            {
                "name": name,
                "sql_sha256": sha256_text(sql),
                "status": "passed" if result["returncode"] == 0 else "failed",
                "duration_ms": result["duration_ms"],
                "stdout": result["stdout"].strip(),
                "stderr": result["stderr"].strip(),
            }
        )
    return results


def run_load(sb: SbRunner, insert_scripts: dict[str, Any]) -> list[dict[str, Any]]:
    results = []
    output_dir = sb.run_root / "load-output"
    for table, meta in insert_scripts.items():
        output_path = output_dir / f"{table}.out"
        before = sb.process_snapshots()
        result = sb.run_file(Path(meta["path"]), output_path=output_path)
        after = sb.process_snapshots()
        rows = count_output_rows(output_path) if output_path.exists() else 0
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
                "stdout_path": str(output_path),
                "stdout_bytes": result["stdout_bytes"],
                "stdout_sha256": result["stdout_sha256"],
                "stderr": result["stderr"].strip(),
                "process_snapshot_before": before,
                "process_snapshot_after": after,
            }
        )
    return results


def verify_data(sb: SbRunner, scale: Scale) -> dict[str, Any]:
    expected = {
        "customers": scale.customers,
        "products": scale.products,
        "orders": scale.orders,
        "order_items": scale.order_items,
    }
    counts = {
        table: sb.scalar(f"SELECT COUNT(*) FROM {sb.full_table(table)}") for table in expected
    }
    integrity_sql = {
        "invalid_order_customers": f"""
SELECT COUNT(*)
FROM {sb.full_table('orders')} o
LEFT JOIN {sb.full_table('customers')} c ON o.customer_id = c.customer_id
WHERE c.customer_id IS NULL""",
        "invalid_item_orders": f"""
SELECT COUNT(*)
FROM {sb.full_table('order_items')} oi
LEFT JOIN {sb.full_table('orders')} o ON oi.order_id = o.order_id
WHERE o.order_id IS NULL""",
        "invalid_item_products": f"""
SELECT COUNT(*)
FROM {sb.full_table('order_items')} oi
LEFT JOIN {sb.full_table('products')} p ON oi.product_id = p.product_id
WHERE p.product_id IS NULL""",
    }
    integrity = {name: sb.scalar(sql) for name, sql in integrity_sql.items()}
    counts_passed = all(counts.get(k) == v for k, v in expected.items())
    integrity_passed = all(value == 0 for value in integrity.values())
    return {
        "expected": expected,
        "actual_counts": counts,
        "integrity": integrity,
        "counts_passed": counts_passed,
        "integrity_passed": integrity_passed,
        "passed": counts_passed and integrity_passed,
    }


def action_sql(sb: SbRunner, action_id: str, scale: Scale) -> str:
    customers = sb.full_table("customers")
    products = sb.full_table("products")
    orders = sb.full_table("orders")
    order_items = sb.full_table("order_items")
    bulk_insert_test = sb.full_table("bulk_insert_test")
    sql = {
        "inner_join_simple": f"""
SELECT o.order_id, o.order_date, c.first_name, c.last_name, c.email
FROM {orders} o
INNER JOIN {customers} c ON o.customer_id = c.customer_id""",
        "inner_join_large_result": f"""
SELECT oi.*, o.order_date, o.status, c.first_name, c.last_name
FROM {order_items} oi
INNER JOIN {orders} o ON oi.order_id = o.order_id
INNER JOIN {customers} c ON o.customer_id = c.customer_id""",
        "inner_join_multiple_conditions": f"""
SELECT o.order_id, o.total_amount, c.country_code
FROM {orders} o
INNER JOIN {customers} c ON o.customer_id = c.customer_id
WHERE o.order_date >= TIMESTAMP '2023-01-01'
  AND o.total_amount > 1000
  AND c.country_code IN ('US', 'CA', 'MX')""",
        "left_join_all_customers": f"""
SELECT c.customer_id,
       c.first_name || ' ' || c.last_name AS full_name,
       COUNT(o.order_id) AS order_count,
       COALESCE(SUM(o.total_amount), 0) AS total_spent
FROM {customers} c
LEFT JOIN {orders} o ON c.customer_id = o.customer_id
GROUP BY c.customer_id, c.first_name, c.last_name""",
        "four_table_join": f"""
SELECT c.customer_id, c.first_name, c.last_name,
       o.order_id, o.order_date, o.total_amount,
       oi.quantity, oi.unit_price,
       p.product_id, p.name AS product_name, p.category
FROM {customers} c
INNER JOIN {orders} o ON c.customer_id = o.customer_id
INNER JOIN {order_items} oi ON o.order_id = oi.order_id
INNER JOIN {products} p ON oi.product_id = p.product_id
WHERE o.order_date >= TIMESTAMP '2024-06-01'""",
        "self_join_same_country": f"""
SELECT c1.customer_id AS customer1_id,
       c1.first_name AS customer1_name,
       c2.customer_id AS customer2_id,
       c2.first_name AS customer2_name,
       c1.country_code
FROM {customers} c1
INNER JOIN {customers} c2 ON c1.country_code = c2.country_code
    AND c1.customer_id < c2.customer_id
WHERE c1.registration_date >= DATE '2024-01-01'
LIMIT 10000""",
        "aggregation_daily_sales": f"""
SELECT DATE_TRUNC('month', o.order_date) AS month,
       p.category,
       COUNT(DISTINCT o.order_id) AS total_orders,
       SUM(oi.quantity) AS total_qty,
       SUM(oi.quantity * oi.unit_price) AS total_revenue,
       AVG(oi.quantity * oi.unit_price) AS avg_line_value
FROM {orders} o
INNER JOIN {order_items} oi ON o.order_id = oi.order_id
INNER JOIN {products} p ON oi.product_id = p.product_id
GROUP BY DATE_TRUNC('month', o.order_date), p.category
HAVING COUNT(DISTINCT o.order_id) >= 10
ORDER BY month DESC, total_revenue DESC""",
        "window_function_ranking": f"""
SELECT c.customer_id,
       c.first_name,
       o.order_id,
       o.total_amount,
       RANK() OVER (PARTITION BY c.customer_id ORDER BY o.total_amount DESC) AS amount_rank,
       COALESCE(SUM(o.total_amount) OVER (PARTITION BY c.customer_id), 0) AS customer_total
FROM {customers} c
INNER JOIN {orders} o ON c.customer_id = o.customer_id
WHERE o.order_date >= TIMESTAMP '2024-01-01'""",
        "multi_dimensional_agg": f"""
SELECT EXTRACT(YEAR FROM o.order_date) AS order_year,
       EXTRACT(MONTH FROM o.order_date) AS order_month,
       c.country_code,
       p.category,
       COUNT(DISTINCT o.order_id) AS orders,
       SUM(oi.quantity) AS units,
       SUM(oi.quantity * oi.unit_price) AS revenue,
       AVG(oi.quantity * oi.unit_price) AS avg_line
FROM {orders} o
INNER JOIN {customers} c ON o.customer_id = c.customer_id
INNER JOIN {order_items} oi ON o.order_id = oi.order_id
INNER JOIN {products} p ON oi.product_id = p.product_id
GROUP BY EXTRACT(YEAR FROM o.order_date),
         EXTRACT(MONTH FROM o.order_date),
         c.country_code,
         p.category
ORDER BY order_year, order_month, revenue DESC""",
        "bulk_insert_select": f"""
INSERT INTO {bulk_insert_test} (id, data, metric_value)
SELECT seq AS id,
       'Data_' || CAST(seq AS VARCHAR(20)) AS data,
       (seq * 1.5) AS metric_value
FROM (
    SELECT ROW_NUMBER() OVER () AS seq
    FROM {orders} o
    CROSS JOIN {order_items} oi
    LIMIT {scale.bulk_insert_rows}
) sub""",
        "bulk_update_with_case": f"""
UPDATE {order_items}
SET discount_pct = CASE
    WHEN quantity >= 50 THEN 20.0
    WHEN quantity >= 20 THEN 15.0
    WHEN quantity >= 10 THEN 10.0
    ELSE 5.0
END
WHERE discount_pct < 5.0 OR discount_pct IS NULL""",
        "bulk_update_with_join": f"""
UPDATE {orders}
SET total_amount = total_amount * 0.95
WHERE customer_id IN (
    SELECT customer_id
    FROM {customers}
    WHERE account_balance > 10000
)""",
        "agg_full_table_scan": f"""
SELECT p.category,
       COUNT(*) AS item_count,
       SUM(oi.quantity) AS total_qty,
       AVG(oi.unit_price) AS avg_price,
       MIN(oi.unit_price) AS min_price,
       MAX(oi.unit_price) AS max_price,
       STDDEV(oi.unit_price) AS price_stddev
FROM {order_items} oi
INNER JOIN {products} p ON oi.product_id = p.product_id
GROUP BY p.category
HAVING COUNT(*) > 1000
ORDER BY item_count DESC""",
        "agg_distinct_counts": f"""
SELECT COUNT(DISTINCT c.customer_id) AS unique_customers,
       COUNT(DISTINCT o.order_id) AS unique_orders,
       COUNT(DISTINCT p.product_id) AS unique_products,
       COUNT(DISTINCT c.country_code) AS countries,
       COUNT(DISTINCT p.category) AS categories
FROM {orders} o
INNER JOIN {customers} c ON o.customer_id = c.customer_id
INNER JOIN {order_items} oi ON o.order_id = oi.order_id
INNER JOIN {products} p ON oi.product_id = p.product_id""",
        "nested_subquery_agg": f"""
SELECT country_stats.country_code,
       country_stats.customer_count,
       country_stats.total_revenue,
       (SELECT AVG(country_revenue) FROM (
            SELECT SUM(total_amount) AS country_revenue
            FROM {orders} o2
            INNER JOIN {customers} c2 ON o2.customer_id = c2.customer_id
            GROUP BY c2.country_code
        ) sub2) AS global_avg,
       country_stats.total_revenue /
        (SELECT AVG(country_revenue) FROM (
            SELECT SUM(total_amount) AS country_revenue
            FROM {orders} o3
            INNER JOIN {customers} c3 ON o3.customer_id = c3.customer_id
            GROUP BY c3.country_code
        ) sub3) AS revenue_ratio
FROM (
    SELECT c.country_code,
           COUNT(DISTINCT c.customer_id) AS customer_count,
           SUM(o.total_amount) AS total_revenue
    FROM {customers} c
    INNER JOIN {orders} o ON c.customer_id = o.customer_id
    GROUP BY c.country_code
) country_stats
ORDER BY country_stats.total_revenue DESC""",
    }[action_id]
    return sql.strip() + ";"


def run_actions(sb: SbRunner, manifest: dict[str, Any]) -> list[dict[str, Any]]:
    output_dir = sb.run_root / "action-output"
    results = []
    for action in manifest["actions"]:
        sql = action_sql(sb, action["id"], sb.scale)
        output_path = output_dir / f"{action['id']}.out"
        before = sb.process_snapshots()
        result = sb.run_sql_text(sql, output_path=output_path)
        after = sb.process_snapshots()
        duration_s = result["duration_ms"] / 1000.0
        rows = count_output_rows(output_path) if result["returncode"] == 0 and output_path.exists() else 0
        rows_returned = rows if action["kind"] == "query" else None
        rows_affected = rows if action["kind"] == "dml" else None
        results.append(
            {
                "action_id": action["id"],
                "kind": action["kind"],
                "sql_sha256": sha256_text(sql),
                "status": "passed" if result["returncode"] == 0 else "failed",
                "duration_ms": result["duration_ms"],
                "rows_returned": rows_returned,
                "rows_affected": rows_affected,
                "rows_per_second": rows / duration_s if duration_s else 0.0,
                "stdout_path": str(output_path),
                "stdout_bytes": result["stdout_bytes"],
                "stdout_sha256": result["stdout_sha256"],
                "stderr": result["stderr"].strip(),
                "process_snapshot_before": before,
                "process_snapshot_after": after,
            }
        )
    return results


def compare_with_postgresql(sb_summary: dict[str, Any], pg_summary_path: Path | None) -> dict[str, Any] | None:
    if pg_summary_path is None:
        return None
    pg = json.loads(pg_summary_path.read_text(encoding="utf-8"))
    pg_actions = {item["action_id"]: item for item in pg.get("actions", [])}
    sb_actions = {item["action_id"]: item for item in sb_summary.get("actions", [])}
    action_rows = []
    for action_id, pg_action in pg_actions.items():
        sb_action = sb_actions.get(action_id)
        pg_rps = float(pg_action.get("rows_per_second") or 0.0)
        sb_rps = float((sb_action or {}).get("rows_per_second") or 0.0)
        target = pg_rps * 0.97
        action_rows.append(
            {
                "action_id": action_id,
                "postgres_status": pg_action.get("status"),
                "scratchbird_status": (sb_action or {}).get("status", "missing"),
                "postgres_rows_per_second": pg_rps,
                "scratchbird_rows_per_second": sb_rps,
                "scratchbird_vs_postgresql_ratio": sb_rps / pg_rps if pg_rps else None,
                "within_three_percent_target": sb_rps >= target if pg_rps and (sb_action or {}).get("status") == "passed" else False,
                "postgres_duration_ms": pg_action.get("duration_ms"),
                "scratchbird_duration_ms": (sb_action or {}).get("duration_ms"),
                "scratchbird_diagnostic": (sb_action or {}).get("stderr", ""),
            }
        )
    pg_load = pg.get("load_summary", {})
    sb_load = sb_summary.get("load_summary", {})
    pg_load_rps = float(pg_load.get("rows_per_second") or 0.0)
    sb_load_rps = float(sb_load.get("rows_per_second") or 0.0)
    return {
        "postgres_summary": str(pg_summary_path),
        "load": {
            "postgres_rows_per_second": pg_load_rps,
            "scratchbird_rows_per_second": sb_load_rps,
            "scratchbird_vs_postgresql_ratio": sb_load_rps / pg_load_rps if pg_load_rps else None,
            "within_three_percent_target": sb_load_rps >= (pg_load_rps * 0.97) if pg_load_rps else False,
        },
        "actions": action_rows,
    }


def write_csv_summary(run_root: Path, actions: list[dict[str, Any]], load: list[dict[str, Any]], comparison: dict[str, Any] | None) -> None:
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
                "stderr",
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
                "script_bytes",
                "duration_ms",
                "rows_per_second",
                "stdout_bytes",
                "stderr",
            ],
        )
        writer.writeheader()
        for row in load:
            writer.writerow({key: row.get(key) for key in writer.fieldnames})

    if comparison is not None:
        with (run_root / "postgres-comparison.csv").open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(
                handle,
                fieldnames=[
                    "action_id",
                    "postgres_status",
                    "scratchbird_status",
                    "postgres_rows_per_second",
                    "scratchbird_rows_per_second",
                    "scratchbird_vs_postgresql_ratio",
                    "within_three_percent_target",
                    "postgres_duration_ms",
                    "scratchbird_duration_ms",
                    "scratchbird_diagnostic",
                ],
            )
            writer.writeheader()
            for row in comparison["actions"]:
                writer.writerow(row)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--latest-server-json", type=Path, default=DEFAULT_LATEST_JSON)
    parser.add_argument("--sbsql", type=Path)
    parser.add_argument("--database")
    parser.add_argument("--host")
    parser.add_argument("--port", type=int)
    parser.add_argument("--sslmode")
    parser.add_argument("--user")
    parser.add_argument("--password")
    parser.add_argument("--role")
    parser.add_argument("--schema", default="")
    parser.add_argument("--table-prefix", default="bench_")
    parser.add_argument("--scale", choices=sorted(SCALES), default="small")
    parser.add_argument("--insert-batch-size", type=int, default=500)
    parser.add_argument("--output-root", type=Path, default=DEFAULT_OUTPUT_ROOT)
    parser.add_argument("--postgres-summary", type=Path)
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    latest = json.loads(args.latest_server_json.read_text(encoding="utf-8"))
    timestamp = utc_timestamp()
    if not args.schema:
        args.schema = f"sb_pgcmp_{args.scale}_{timestamp.lower().replace('z', '')}"
    validate_ident(args.schema)
    scale = SCALES[args.scale]
    run_root = args.output_root / f"scratchbird-{args.scale}-{timestamp}"
    run_root.mkdir(parents=True, exist_ok=False)
    sb = SbRunner(args, run_root, scale, latest)

    manifest = json.loads((SUITE_ROOT / "manifest.json").read_text(encoding="utf-8"))
    started = time.perf_counter()

    version = subprocess.run(
        [str(sb.sbsql), "--version"],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    provenance = {
        "schema_version": "scratchbird.reference_regression.scratchbird.apples_to_apples.result.v1",
        "timestamp_utc": timestamp,
        "suite_root": str(SUITE_ROOT),
        "run_root": str(run_root),
        "scale": args.scale,
        "scale_counts": scale.__dict__,
        "connection": {
            "host": sb.host,
            "port": sb.port,
            "database": sb.database,
            "user": sb.user,
            "role": sb.role,
            "sslmode": sb.sslmode,
            "schema": sb.full_schema,
        },
        "sbsql_version": version.stdout.strip() or version.stderr.strip(),
        "manifest_sha256": sha256_file(SUITE_ROOT / "manifest.json"),
        "latest_server_json": str(args.latest_server_json),
        "table_prefix": args.table_prefix,
        "server_runtime_root": latest.get("runtime_root"),
        "server_stdout": latest.get("server_stdout"),
        "server_stderr": latest.get("server_stderr"),
        "listener_stdout": latest.get("listener_stdout"),
        "listener_stderr": latest.get("listener_stderr"),
        "engine_sql_text_execution": False,
        "parser_output_to_engine_required": True,
        "mga_authority": "engine",
    }

    pre_snapshot = {
        "platform": {
            "system": platform.system(),
            "release": platform.release(),
            "machine": platform.machine(),
            "processor": platform.processor(),
            "python": platform.python_version(),
        },
        "processes": sb.process_snapshots(),
    }
    setup = run_setup(sb)
    data_files = generate_data_files(run_root, scale)
    insert_scripts = generate_insert_scripts(run_root, sb, scale, args.insert_batch_size)
    load_results: list[dict[str, Any]] = []
    index_setup: list[dict[str, Any]] = []
    verification: dict[str, Any] = {"passed": False, "skipped": "setup failed"}
    action_results: list[dict[str, Any]] = []

    if all(item["status"] == "passed" for item in setup):
        load_results = run_load(sb, insert_scripts)
        index_setup = run_index_setup(sb)
        verification = verify_data(sb, scale)
        action_results = run_actions(sb, manifest)

    total_load_rows = sum(item.get("rows", 0) for item in load_results)
    total_load_ms = sum(item.get("duration_ms", 0.0) for item in load_results)
    elapsed_ms = (time.perf_counter() - started) * 1000.0
    status = "passed"
    if any(item["status"] != "passed" for item in setup):
        status = "failed"
    if any(item["status"] != "passed" for item in load_results):
        status = "failed"
    if any(item["status"] != "passed" for item in index_setup):
        status = "failed"
    if not verification.get("passed", False):
        status = "failed"
    if any(item["status"] != "passed" for item in action_results):
        status = "failed"

    summary = {
        "provenance": provenance,
        "status": status,
        "elapsed_ms": elapsed_ms,
        "setup": setup,
        "data_files": data_files,
        "insert_scripts": insert_scripts,
        "load_results": load_results,
        "load_summary": {
            "rows": total_load_rows,
            "duration_ms": total_load_ms,
            "rows_per_second": total_load_rows / (total_load_ms / 1000.0) if total_load_ms else 0.0,
        },
        "index_setup": index_setup,
        "verification": verification,
        "actions": action_results,
        "pre_snapshot": pre_snapshot,
        "post_snapshot": {"processes": sb.process_snapshots()},
    }
    comparison = compare_with_postgresql(summary, args.postgres_summary)
    if comparison is not None:
        summary["postgres_comparison"] = comparison

    (run_root / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    write_csv_summary(run_root, action_results, load_results, comparison)
    print(json.dumps({"status": status, "run_root": str(run_root), "summary": str(run_root / "summary.json")}, indent=2))
    return 0 if status == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
