#!/usr/bin/env python3
"""
Dialect-Aware Stress Test Runner

Executes stress tests using engine-specific SQL dialects:
- FirebirdSQL dialect 3
- MySQL 8.0+ dialect
- PostgreSQL dialect

This ensures fair comparison - each engine is tested with its native SQL.
"""

import argparse
import json
import os
import shutil
import sys
import time
import traceback
from dataclasses import asdict, dataclass, field
from datetime import datetime
from decimal import Decimal
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

# Add parent directory to path for imports
PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).parent.parent))
scratchbird_driver_candidates = [
    PROJECT_ROOT.parents[3] / "project" / "drivers" / "driver" / "python" / "src",
]
if os.environ.get("SCRATCHBIRD_DRIVER_PYTHONPATH"):
    scratchbird_driver_candidates.insert(0, Path(os.environ["SCRATCHBIRD_DRIVER_PYTHONPATH"]))
for scratchbird_driver in scratchbird_driver_candidates:
    if scratchbird_driver.exists():
        sys.path.insert(0, str(scratchbird_driver))
        break

from generators.data_generator import (
    TableDataGenerator,
    generate_standard_dataset,
    generate_verification_queries
)
from generators.sql_dialect import SQLDialectFactory, StressTestSQLGenerator
from scenarios.dialect_aware_tests import (
    get_tests_for_engine,
    DialectAwareJoinTests,
    DialectAwareBulkTests
)


@dataclass
class TestMetrics:
    """Metrics collected during test execution."""
    test_name: str
    description: str
    status: str = "pending"
    start_time: Optional[float] = None
    end_time: Optional[float] = None
    duration_ms: float = 0.0
    rows_affected: int = 0
    rows_returned: int = 0
    error_message: str = ""
    verification_passed: bool = False
    sql_executed: str = ""


@dataclass
class DataLoadMetrics:
    """Metrics for data loading phase."""
    table_name: str
    row_count: int
    start_time: float = 0.0
    end_time: float = 0.0
    duration_ms: float = 0.0
    rows_per_second: float = 0.0
    status: str = "pending"
    error_message: str = ""


def _sanitize_artifact_name(value: str) -> str:
    return "".join(ch if ch.isalnum() or ch in ("-", "_") else "_" for ch in value).strip("_") or "artifact"


def _sql_starts_with_write(sql: str) -> bool:
    normalized = sql.lstrip().upper()
    return normalized.startswith(("INSERT", "UPDATE", "DELETE", "MERGE"))


def _coerce_trace_value(raw: str) -> Any:
    lowered = raw.lower()
    if lowered in ("true", "false"):
        return lowered == "true"
    try:
        if any(ch in raw for ch in (".", "e", "E")):
            return float(raw)
        return int(raw)
    except ValueError:
        return raw


def _duration_value_ms(field: str, value: Any) -> Optional[float]:
    if not isinstance(value, (int, float)):
        return None
    if field.endswith("_us"):
        return float(value) / 1000.0
    if field.endswith("_ms") or field in ("duration_ms", "total_ms", "execution_time_ms"):
        return float(value)
    return None


def _parse_trace_line(line: str) -> Dict[str, Any]:
    tokens = line.strip().split()
    prefix_tokens: List[str] = []
    field_tokens: List[str] = []
    seen_fields = False
    for token in tokens:
        if "=" in token:
            seen_fields = True
        if seen_fields:
            field_tokens.append(token)
        else:
            prefix_tokens.append(token)

    fields: Dict[str, Any] = {}
    for token in field_tokens:
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        fields[key] = _coerce_trace_value(value)

    return {
        "prefix": " ".join(prefix_tokens),
        "fields": fields,
    }


def _trace_group_label(prefix: str, fields: Dict[str, Any]) -> str:
    for key in ("table", "mode", "kind", "scan_kind", "runtime_access", "handoff"):
        if key in fields:
            return f"{prefix} {key}={fields[key]}"
    return prefix


def summarize_trace_file(path: Path) -> Dict[str, Any]:
    if not path.exists():
        return {
            "path": str(path),
            "exists": False,
            "line_count": 0,
            "groups": [],
            "top_time_contributors": [],
        }

    groups: Dict[str, Dict[str, Any]] = {}
    contributors: List[Dict[str, Any]] = []
    line_count = 0

    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for raw_line in handle:
            line = raw_line.strip()
            if not line:
                continue
            line_count += 1
            parsed = _parse_trace_line(line)
            prefix = parsed["prefix"]
            fields = parsed["fields"]
            label = _trace_group_label(prefix, fields)
            group = groups.setdefault(
                label,
                {
                    "label": label,
                    "prefix": prefix,
                    "line_count": 0,
                    "numeric_totals": {},
                    "numeric_max": {},
                    "sample_fields": {},
                },
            )
            group["line_count"] += 1

            for key, value in fields.items():
                if key not in group["sample_fields"]:
                    group["sample_fields"][key] = value
                if isinstance(value, (int, float)):
                    group["numeric_totals"][key] = group["numeric_totals"].get(key, 0) + value
                    group["numeric_max"][key] = max(group["numeric_max"].get(key, value), value)
                    duration_ms = _duration_value_ms(key, value)
                    if duration_ms is not None and duration_ms > 0:
                        contributors.append(
                            {
                                "group": label,
                                "field": key,
                                "duration_ms": duration_ms,
                            }
                        )

    top_groups: List[Dict[str, Any]] = []
    for group in groups.values():
        total_duration_ms = 0.0
        for key, value in group["numeric_totals"].items():
            duration_ms = _duration_value_ms(key, value)
            if duration_ms is not None:
                total_duration_ms += duration_ms
        group["total_duration_ms"] = total_duration_ms
        top_groups.append(group)

    top_groups.sort(key=lambda item: (item["total_duration_ms"], item["line_count"]), reverse=True)
    contributors.sort(key=lambda item: item["duration_ms"], reverse=True)

    return {
        "path": str(path),
        "exists": True,
        "line_count": line_count,
        "groups": top_groups[:25],
        "top_time_contributors": contributors[:50],
    }


def _collect_plan_nodes(node: Dict[str, Any], depth: int, nodes: List[Dict[str, Any]]) -> None:
    entry = {
        "depth": depth,
        "node_type": node.get("Node Type"),
        "relation": node.get("Relation Name"),
        "index_name": node.get("Index Name"),
        "actual_rows": node.get("Actual Rows"),
        "actual_loops": node.get("Actual Loops"),
        "actual_total_time_ms": node.get("Actual Total Time"),
        "actual_startup_time_ms": node.get("Actual Startup Time"),
        "plan_rows": node.get("Plan Rows"),
        "shared_hit_blocks": node.get("Shared Hit Blocks"),
        "shared_read_blocks": node.get("Shared Read Blocks"),
        "temp_read_blocks": node.get("Temp Read Blocks"),
        "temp_written_blocks": node.get("Temp Written Blocks"),
    }
    nodes.append(entry)
    for child in node.get("Plans", []):
        _collect_plan_nodes(child, depth + 1, nodes)


def summarize_postgresql_explain_json(path: Path) -> Dict[str, Any]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if not payload or not isinstance(payload, list):
        return {"path": str(path), "exists": True, "node_count": 0, "top_nodes": []}

    root = payload[0]
    plan = root.get("Plan", {})
    nodes: List[Dict[str, Any]] = []
    if isinstance(plan, dict):
        _collect_plan_nodes(plan, 0, nodes)
    top_nodes = sorted(
        [node for node in nodes if isinstance(node.get("actual_total_time_ms"), (int, float))],
        key=lambda item: item["actual_total_time_ms"],
        reverse=True,
    )
    return {
        "path": str(path),
        "exists": True,
        "planning_time_ms": root.get("Planning Time"),
        "execution_time_ms": root.get("Execution Time"),
        "node_count": len(nodes),
        "top_nodes": top_nodes[:25],
    }


class DatabaseConnection:
    """Database connection wrapper supporting multiple engines."""

    def __init__(self, engine: str, host: str, port: int, database: str,
                 user: str, password: str, transaction_mode: str = "engine_default"):
        self.engine = engine
        self.host = host
        self.port = port
        self.database = database
        self.user = user
        self.password = password
        self.requested_transaction_mode = transaction_mode
        self.effective_transaction_mode = "normal_transactional"
        self.connection_handles_statement_commit = False
        self.explicit_commit_per_statement = False
        self.connection = None
        self.cursor = None

        self._connect()

    def _normalized_transaction_mode(self) -> str:
        legacy_aliases = {
            "always_in_transaction": "normal_transactional",
            "autocommit_statement": "no_transaction",
        }
        requested = legacy_aliases.get(
            self.requested_transaction_mode, self.requested_transaction_mode
        )
        if requested == "engine_default":
            if self.engine in ("mysql", "postgresql"):
                return "no_transaction"
            return "normal_transactional"
        return requested

    def _safe_rollback(self) -> None:
        try:
            self.connection.rollback()
        except Exception:
            pass

    def _connect(self):
        """Establish database connection."""
        self.effective_transaction_mode = self._normalized_transaction_mode()
        if self.engine in ("firebird", "scratchbird"):
            if self.effective_transaction_mode not in ("normal_transactional", "autocommit"):
                raise ValueError(
                    f"{self.engine} supports only normal_transactional or autocommit"
                )
        elif self.engine in ("mysql", "postgresql"):
            if self.effective_transaction_mode not in (
                "no_transaction",
                "normal_transactional",
                "autocommit",
            ):
                raise ValueError(
                    f"Unsupported transaction mode '{self.requested_transaction_mode}' for {self.engine}"
                )
        else:
            raise ValueError(f"Unsupported engine: {self.engine}")

        if self.engine == "firebird":
            import fdb
            self.connection = fdb.connect(
                host=self.host,
                port=self.port,
                database=self.database,
                user=self.user,
                password=self.password
            )
            self.explicit_commit_per_statement = (
                self.effective_transaction_mode == "autocommit"
            )
        elif self.engine == "mysql":
            import pymysql
            self.connection = pymysql.connect(
                host=self.host,
                port=self.port,
                database=self.database,
                user=self.user,
                password=self.password,
                charset='utf8mb4',
                autocommit=(self.effective_transaction_mode == "no_transaction")
            )
            if self.effective_transaction_mode == "no_transaction":
                self.connection_handles_statement_commit = True
            elif self.effective_transaction_mode == "autocommit":
                self.explicit_commit_per_statement = True
        elif self.engine == "postgresql":
            import psycopg2
            self.connection = psycopg2.connect(
                host=self.host,
                port=self.port,
                dbname=self.database,
                user=self.user,
                password=self.password
            )
            self.connection.autocommit = (self.effective_transaction_mode == "no_transaction")
            if self.effective_transaction_mode == "no_transaction":
                self.connection_handles_statement_commit = True
            elif self.effective_transaction_mode == "autocommit":
                self.explicit_commit_per_statement = True
        elif self.engine == "scratchbird":
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
            self.connection.autocommit = (self.effective_transaction_mode == "autocommit")
            if self.effective_transaction_mode == "autocommit":
                self.connection_handles_statement_commit = True
            leak_detector = getattr(self.connection, "_leak_detector", None)
            if leak_detector is not None and hasattr(leak_detector, "config"):
                # Stress benchmarks intentionally hold a single session open while
                # loading and exercising the dataset. Treat that as expected up to
                # a multi-hour window so the log stays focused on real failures.
                leak_detector.config.threshold = max(float(leak_detector.config.threshold), 7200.0)
        self.cursor = self.connection.cursor()

    def execute(self, sql: str, params: Optional[Tuple] = None) -> Any:
        """Execute SQL statement."""
        try:
            if params:
                self.cursor.execute(sql, params)
            else:
                self.cursor.execute(sql)
            if self.explicit_commit_per_statement and self.cursor.description is None:
                self.connection.commit()
            return self.cursor
        except Exception:
            self._safe_rollback()
            raise

    def executemany(self, sql: str, seq_of_params) -> Any:
        """Execute a batch of parameterized statements."""
        try:
            batch = list(seq_of_params)
            if (
                self.engine == "postgresql"
                and batch
                and sql.lstrip().upper().startswith("INSERT INTO ")
            ):
                from psycopg2.extras import execute_values

                head, marker, tail = sql.partition("VALUES")
                if marker:
                    execute_values(
                        self.cursor,
                        f"{head}VALUES %s",
                        batch,
                        template=tail.strip(),
                        page_size=len(batch),
                    )
                else:
                    self.cursor.executemany(sql, batch)
            else:
                self.cursor.executemany(sql, batch)
            if self.explicit_commit_per_statement:
                self.connection.commit()
            return self.cursor
        except Exception:
            self._safe_rollback()
            raise

    def commit(self):
        """Commit transaction."""
        if self.effective_transaction_mode != "normal_transactional":
            return
        self.connection.commit()

    def rollback(self):
        """Rollback transaction."""
        if self.effective_transaction_mode != "normal_transactional":
            return
        self.connection.rollback()

    def fetchall(self) -> List[Tuple]:
        """Fetch all results."""
        rows = self.cursor.fetchall()
        if self.explicit_commit_per_statement:
            self.connection.commit()
        return rows

    def fetchone(self) -> Optional[Tuple]:
        """Fetch one result."""
        row = self.cursor.fetchone()
        if self.explicit_commit_per_statement:
            self.connection.commit()
        return row

    def rowcount(self) -> int:
        """Get row count from last operation."""
        return self.cursor.rowcount

    def close(self):
        """Close connection."""
        if self.cursor:
            self.cursor.close()
        if self.connection:
            self.connection.close()


class DialectStressTestRunner:
    """Main dialect-aware stress test runner."""

    def __init__(self, engine: str, host: str, port: int, database: str,
                 user: str, password: str, output_dir: Path,
                 transaction_mode: str = "engine_default",
                 diagnostics_dir: Optional[Path] = None,
                 capture_scratchbird_traces: bool = False,
                 capture_postgresql_explain: bool = False):
        self.engine = engine
        self.host = host
        self.port = port
        self.database = database
        self.user = user
        self.password = password
        self.output_dir = output_dir
        self.requested_transaction_mode = transaction_mode
        self.effective_transaction_mode = transaction_mode
        self.diagnostics_dir = diagnostics_dir
        self.capture_scratchbird_traces = capture_scratchbird_traces
        self.capture_postgresql_explain = capture_postgresql_explain

        self.db: Optional[DatabaseConnection] = None
        self.dialect = SQLDialectFactory.get_dialect(engine)
        self.sql_gen = StressTestSQLGenerator(self.dialect)
        self.metrics: List[TestMetrics] = []
        self.load_metrics: List[DataLoadMetrics] = []
        self.phase_diagnostics: List[Dict[str, Any]] = []

        self.output_dir.mkdir(parents=True, exist_ok=True)
        if self.diagnostics_dir is not None:
            self.diagnostics_dir.mkdir(parents=True, exist_ok=True)

    def _load_batch_size(self, requested_batch_size: int) -> int:
        """Keep ScratchBird load transactions small enough for Beta 1 gates."""
        if self.engine == "scratchbird":
            return min(requested_batch_size, 4096)
        return requested_batch_size

    def _live_scratchbird_trace_paths(self) -> Dict[str, Path]:
        paths: Dict[str, Path] = {}
        env_map = {
            "native_bind_trace": "SCRATCHBIRD_NATIVE_BIND_TRACE_FILE",
            "select_trace": "SCRATCHBIRD_SELECT_TRACE_FILE",
            "storage_insert_trace": "SCRATCHBIRD_INSERT_TRACE_FILE",
            "executor_insert_trace": "SCRATCHBIRD_EXECUTOR_INSERT_TRACE_FILE",
            "update_trace": "SCRATCHBIRD_UPDATE_TRACE_FILE",
            "exec_profile_trace": "SCRATCHBIRD_EXEC_PROFILE_FILE",
            "prepared_trace": "SCRATCHBIRD_PREPARED_TRACE_FILE",
        }
        for name, env_key in env_map.items():
            raw = os.getenv(env_key)
            if raw:
                paths[name] = Path(raw)
        return paths

    def _begin_phase_diagnostics(self, phase_kind: str, phase_name: str) -> Dict[str, Any]:
        phase_slug = f"{phase_kind}_{_sanitize_artifact_name(phase_name)}"
        phase_entry: Dict[str, Any] = {
            "phase_kind": phase_kind,
            "phase_name": phase_name,
            "phase_slug": phase_slug,
            "engine": self.engine,
        }
        if self.diagnostics_dir is None:
            return phase_entry

        phase_dir = self.diagnostics_dir / phase_slug
        phase_dir.mkdir(parents=True, exist_ok=True)
        phase_entry["phase_dir"] = str(phase_dir)

        if self.engine == "scratchbird" and self.capture_scratchbird_traces:
            live_paths = self._live_scratchbird_trace_paths()
            phase_entry["live_trace_files"] = {name: str(path) for name, path in live_paths.items()}
            for path in live_paths.values():
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("", encoding="utf-8")

        return phase_entry

    def _capture_postgresql_explain(self, sql: str, phase_dir: Path) -> Optional[Dict[str, Any]]:
        if not self.capture_postgresql_explain or self.engine != "postgresql":
            return None

        import psycopg2

        conn = psycopg2.connect(
            host=self.host,
            port=self.port,
            dbname=self.database,
            user=self.user,
            password=self.password,
        )
        conn.autocommit = False
        try:
            with conn.cursor() as cursor:
                cursor.execute("EXPLAIN (ANALYZE, BUFFERS, VERBOSE, FORMAT JSON) " + sql)
                row = cursor.fetchone()
                payload = row[0] if row else []
                explain_path = phase_dir / "postgresql_explain.json"
                explain_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
                return summarize_postgresql_explain_json(explain_path)
        except Exception as exc:
            error_path = phase_dir / "postgresql_explain_error.txt"
            error_path.write_text(str(exc), encoding="utf-8")
            return {
                "path": str(phase_dir / "postgresql_explain.json"),
                "exists": False,
                "error": str(exc),
            }
        finally:
            try:
                conn.rollback()
            except Exception:
                pass
            conn.close()

    def _capture_postgresql_sample_insert_explain(
        self,
        table_name: str,
        columns: List[str],
        batch_values: List[Tuple[Any, ...]],
        phase_dir: Path,
    ) -> Optional[Dict[str, Any]]:
        if not self.capture_postgresql_explain or self.engine != "postgresql" or not batch_values:
            return None

        sample_rows = batch_values[: min(len(batch_values), 32)]
        values_sql = []
        for row in sample_rows:
            values_sql.append(
                "(" + ", ".join(self._sql_literal(value) for value in row) + ")"
            )
        sql = (
            f"INSERT INTO {table_name} ({', '.join(columns)}) VALUES "
            + ", ".join(values_sql)
        )
        return self._capture_postgresql_explain(sql, phase_dir)

    def _finish_phase_diagnostics(
        self,
        phase_entry: Dict[str, Any],
        *,
        status: str,
        extra: Optional[Dict[str, Any]] = None,
    ) -> None:
        phase_entry["status"] = status
        if extra:
            phase_entry.update(extra)

        phase_dir_value = phase_entry.get("phase_dir")
        if phase_dir_value:
            phase_dir = Path(phase_dir_value)
            if self.engine == "scratchbird" and self.capture_scratchbird_traces:
                copied_logs: Dict[str, Any] = {}
                for name, live_path in self._live_scratchbird_trace_paths().items():
                    if not live_path.exists():
                        continue
                    captured_path = phase_dir / f"{name}.log"
                    shutil.copy2(live_path, captured_path)
                    copied_logs[name] = summarize_trace_file(captured_path)
                phase_entry["scratchbird_trace_summary"] = copied_logs

        self.phase_diagnostics.append(phase_entry)

    def save_diagnostics_summary(self) -> Optional[Path]:
        if self.diagnostics_dir is None:
            return None
        summary_path = self.diagnostics_dir / "diagnostics-summary.json"
        summary_payload = {
            "engine": self.engine,
            "requested_transaction_mode": self.requested_transaction_mode,
            "effective_transaction_mode": self.effective_transaction_mode,
            "phases": self.phase_diagnostics,
        }
        summary_path.write_text(json.dumps(summary_payload, indent=2), encoding="utf-8")
        return summary_path

    @staticmethod
    def _workload_index_ddls() -> List[str]:
        """Secondary indexes required for query-phase stress coverage."""
        return [
            "CREATE INDEX idx_stress_customers_country_customer ON customers (country_code, customer_id)",
            "CREATE INDEX idx_stress_customers_registration ON customers (registration_date)",
            "CREATE INDEX idx_stress_customers_balance ON customers (account_balance)",
            "CREATE INDEX idx_stress_orders_customer_date ON orders (customer_id, order_date)",
            "CREATE INDEX idx_stress_orders_order_date ON orders (order_date)",
            "CREATE INDEX idx_stress_order_items_order_id ON order_items (order_id)",
            "CREATE INDEX idx_stress_order_items_product_id ON order_items (product_id)",
            "CREATE INDEX idx_stress_products_category ON products (category)",
        ]

    @staticmethod
    def _sql_literal(value: Any) -> str:
        if value is None:
            return "NULL"
        if isinstance(value, bool):
            return "1" if value else "0"
        if isinstance(value, (int, float, Decimal)):
            return str(value)
        if isinstance(value, datetime):
            return f"'{value:%Y-%m-%d %H:%M:%S}'"
        text = str(value).replace("'", "''")
        return f"'{text}'"

    def export_load_sql(self, dataset: Dict[str, Any], script_path: Path, batch_size: int = 10000):
        """Export schema plus load batches as a plain SQL script."""
        print(f"\nWriting SQL load script to {script_path}...")
        batch_size = self._load_batch_size(batch_size)
        script_path.parent.mkdir(parents=True, exist_ok=True)

        with script_path.open("w", encoding="utf-8") as handle:
            handle.write("-- Generated by dialect_stress_runner.py\n")
            handle.write(f"-- engine={self.engine} scale export batch_size={batch_size}\n\n")
            handle.write("DROP TABLE IF EXISTS bulk_insert_test;\n")
            handle.write("DROP TABLE IF EXISTS order_items;\n")
            handle.write("DROP TABLE IF EXISTS orders;\n")
            handle.write("DROP TABLE IF EXISTS products;\n")
            handle.write("DROP TABLE IF EXISTS customers;\n")
            handle.write("COMMIT;\n\n")
            handle.write(self.dialect.create_table_customers().strip())
            handle.write(";\n")
            handle.write(self.dialect.create_table_products().strip())
            handle.write(";\n")
            handle.write(self.dialect.create_table_orders().strip())
            handle.write(";\n")
            handle.write(self.dialect.create_table_order_items().strip())
            handle.write(";\n")
            handle.write(
                "CREATE TABLE bulk_insert_test (\n"
                "    id BIGINT PRIMARY KEY,\n"
                "    data VARCHAR(100),\n"
                "    metric_value DECIMAL(10, 2)\n"
                ");\n"
            )
            handle.write("COMMIT;\n\n")

            fk_references = {}
            for table_name, spec in dataset.items():
                print(f"  Exporting {table_name} ({spec.row_count:,} rows)...")
                generator = TableDataGenerator(spec, fk_references)
                columns = [c.name for c in spec.columns]
                column_list = ", ".join(columns)
                rows_written = 0

                for batch_num, batch in enumerate(generator.generate_rows(batch_size)):
                    if batch_num % 10 == 0:
                        print(f"    Batch {batch_num}: {rows_written:,} rows scripted...")
                    values_sql = []
                    for row in batch:
                        values_sql.append(
                            "(" + ", ".join(self._sql_literal(row[c]) for c in columns) + ")"
                        )
                    handle.write(
                        f"INSERT INTO {table_name} ({column_list}) VALUES\n"
                        f"  " + ",\n  ".join(values_sql) + ";\n"
                    )
                    handle.write("COMMIT;\n")
                    rows_written += len(batch)

                pk_col = next((c for c in spec.columns if c.unique and c.distribution == "sequential"), None)
                if pk_col:
                    fk_references[f"{table_name}.{pk_col.name}"] = list(range(1, spec.row_count + 1))
                handle.write("\n")

            handle.write("-- workload indexes built after load to keep insert timing separate\n")
            for ddl in self._workload_index_ddls():
                handle.write(ddl)
                handle.write(";\n")
            handle.write("COMMIT;\n")

        print(f"SQL load script written: {script_path}")

    def connect(self):
        """Connect to database."""
        print(f"Connecting to {self.engine} at {self.host}:{self.port}...")
        print(f"Using {self.engine.upper()} SQL dialect")
        self.db = DatabaseConnection(
            self.engine, self.host, self.port,
            self.database, self.user, self.password,
            transaction_mode=self.requested_transaction_mode
        )
        self.effective_transaction_mode = self.db.effective_transaction_mode
        print(f"Transaction mode: {self.effective_transaction_mode}")
        print("Connected.")

    def disconnect(self):
        """Disconnect from database."""
        if self.db:
            self.db.close()
            self.db = None
            print("Disconnected.")

    def create_schema(self, dataset: Dict[str, Any]):
        """Create database schema using dialect-specific SQL."""
        print(f"\nCreating schema using {self.engine} dialect...")
        phase_entry = self._begin_phase_diagnostics("schema", "create_schema")
        phase_start = time.time()

        # Drop existing tables in dependency-safe order.
        tables = ["bulk_insert_test", "order_items", "orders", "products", "customers"]
        for table in tables:
            try:
                self.db.execute(f"DROP TABLE {table}")
                self.db.commit()
            except:
                pass

        # Create tables using dialect-specific DDL
        self.db.execute(self.dialect.create_table_customers())
        self.db.execute(self.dialect.create_table_products())
        self.db.execute(self.dialect.create_table_orders())
        self.db.execute(self.dialect.create_table_order_items())
        self.db.execute("""
            CREATE TABLE bulk_insert_test (
                id BIGINT PRIMARY KEY,
                data VARCHAR(100),
                metric_value DECIMAL(10, 2)
            )
        """)

        self.db.commit()
        print("Schema created.")
        self._finish_phase_diagnostics(
            phase_entry,
            status="success",
            extra={
                "duration_ms": (time.time() - phase_start) * 1000,
                "tables": tables,
            },
        )

    def load_data(self, dataset: Dict[str, Any], batch_size: int = 10000):
        """Load generated data into database."""
        print(f"\nLoading data using {self.engine} dialect...")
        batch_size = self._load_batch_size(batch_size)
        self.current_load_batch_size = batch_size

        placeholder = self.dialect.get_placeholder()
        fk_references = {}

        for table_name, spec in dataset.items():
            print(f"\n  Loading {table_name} ({spec.row_count:,} rows)...")
            phase_entry = self._begin_phase_diagnostics("load", table_name)

            metric = DataLoadMetrics(
                table_name=table_name,
                row_count=spec.row_count
            )
            metric.start_time = time.time()

            try:
                generator = TableDataGenerator(spec, fk_references)

                columns = [c.name for c in spec.columns]
                placeholders = ", ".join([placeholder] * len(columns))
                sql = f"INSERT INTO {table_name} ({', '.join(columns)}) VALUES ({placeholders})"

                rows_loaded = 0
                sample_explain: Optional[Dict[str, Any]] = None
                for batch_num, batch in enumerate(generator.generate_rows(batch_size)):
                    if batch_num % 10 == 0:
                        print(f"    Batch {batch_num}: {rows_loaded:,} rows loaded...")

                    batch_values = [tuple(row[c] for c in columns) for row in batch]
                    phase_dir_value = phase_entry.get("phase_dir")
                    if (
                        sample_explain is None
                        and phase_dir_value
                        and self.engine == "postgresql"
                    ):
                        sample_explain = self._capture_postgresql_sample_insert_explain(
                            table_name,
                            columns,
                            batch_values,
                            Path(phase_dir_value),
                        )
                    self.db.executemany(sql, batch_values)

                    self.db.commit()
                    rows_loaded += len(batch)

                metric.end_time = time.time()
                metric.duration_ms = (metric.end_time - metric.start_time) * 1000
                metric.rows_per_second = spec.row_count / (metric.duration_ms / 1000)
                metric.status = "success"

                print(f"    Loaded {rows_loaded:,} rows in {metric.duration_ms/1000:.2f}s "
                      f"({metric.rows_per_second:,.0f} rows/sec)")
                phase_extra = {
                    "duration_ms": metric.duration_ms,
                    "row_count": rows_loaded,
                    "rows_per_second": metric.rows_per_second,
                    "sample_sql": sql,
                }
                if sample_explain is not None:
                    phase_extra["postgresql_sample_insert_explain"] = sample_explain

                # Store reference values for FK relationships
                pk_col = next((c for c in spec.columns if c.unique and c.distribution == "sequential"), None)
                if pk_col:
                    fk_references[f"{table_name}.{pk_col.name}"] = list(range(1, spec.row_count + 1))
                self._finish_phase_diagnostics(phase_entry, status="success", extra=phase_extra)

            except Exception as e:
                metric.end_time = time.time()
                metric.status = "failed"
                metric.error_message = str(e)
                print(f"    ERROR: {e}")
                self._finish_phase_diagnostics(
                    phase_entry,
                    status="failed",
                    extra={
                        "duration_ms": (metric.end_time - metric.start_time) * 1000,
                        "error_message": str(e),
                        "sample_sql": sql,
                    },
                )

            self.load_metrics.append(metric)

        print("\nData loading complete.")

    def create_workload_indexes(self):
        """Build the query-phase workload indexes after data load."""
        print(f"\nCreating workload indexes using {self.engine} dialect...")
        start = time.time()
        phase_entry = self._begin_phase_diagnostics("indexes", "workload_indexes")

        for ddl in self._workload_index_ddls():
            self.db.execute(ddl)

        self.db.commit()
        duration_ms = (time.time() - start) * 1000
        print(f"Workload indexes created in {duration_ms/1000:.2f}s")
        self._finish_phase_diagnostics(
            phase_entry,
            status="success",
            extra={
                "duration_ms": duration_ms,
                "index_count": len(self._workload_index_ddls()),
                "ddls": self._workload_index_ddls(),
            },
        )

    def verify_data(self, dataset: Dict[str, Any]) -> bool:
        """Run verification queries to ensure data integrity."""
        print("\nVerifying data integrity...")

        queries = generate_verification_queries(dataset)
        all_passed = True

        for vq in queries:
            print(f"  {vq['name']}: ", end="", flush=True)
            phase_entry = self._begin_phase_diagnostics("verify", vq["name"])
            phase_start = time.time()

            try:
                self.db.execute(vq['sql'])
                result = self.db.fetchone()
                actual_value = result[0] if result else None

                expected = vq['expected']
                tolerance = vq.get('tolerance', 0)

                if tolerance == 0:
                    passed = (actual_value == expected)
                else:
                    passed = abs(actual_value - expected) <= tolerance

                if passed:
                    print(f"PASS (expected={expected}, actual={actual_value})")
                else:
                    print(f"FAIL (expected={expected}, actual={actual_value})")
                    all_passed = False
                self._finish_phase_diagnostics(
                    phase_entry,
                    status="success" if passed else "failed",
                    extra={
                        "duration_ms": (time.time() - phase_start) * 1000,
                        "sql": vq["sql"],
                        "expected": expected,
                        "actual": actual_value,
                        "tolerance": tolerance,
                    },
                )

            except Exception as e:
                print(f"ERROR: {e}")
                all_passed = False
                self._finish_phase_diagnostics(
                    phase_entry,
                    status="error",
                    extra={
                        "duration_ms": (time.time() - phase_start) * 1000,
                        "sql": vq["sql"],
                        "error_message": str(e),
                    },
                )

        return all_passed

    def run_test(self, test_name: str, test_def: Dict[str, Any],
                 timeout_seconds: int = 300) -> TestMetrics:
        """Run a single stress test with dialect-specific SQL."""
        sql = test_def.get('sql', '')
        if not sql:
            return TestMetrics(
                test_name=test_name,
                description=test_def.get('description', ''),
                status="error",
                error_message="No SQL generated for this test"
            )

        metric = TestMetrics(
            test_name=test_name,
            description=test_def.get('description', ''),
            sql_executed=sql[:500]  # Store first 500 chars
        )

        print(f"\nRunning: {test_name}")
        print(f"  Description: {test_def.get('description', '')}")
        print(f"  Timeout: {test_def.get('timeout_seconds', 300)}s")
        phase_entry = self._begin_phase_diagnostics("test", test_name)

        metric.start_time = time.time()
        metric.status = "running"
        phase_extra: Dict[str, Any] = {
            "sql": sql,
        }

        phase_dir_value = phase_entry.get("phase_dir")
        if phase_dir_value and self.engine == "postgresql" and _sql_starts_with_write(sql):
            phase_extra["postgresql_explain"] = self._capture_postgresql_explain(
                sql,
                Path(phase_dir_value),
            )

        try:
            self.db.execute(sql)

            if self.db.cursor.description:
                rows = self.db.fetchall()
                metric.rows_returned = len(rows)
                print(f"  Rows returned: {metric.rows_returned:,}")
            else:
                metric.rows_affected = self.db.rowcount()
                print(f"  Rows affected: {metric.rows_affected:,}")

            metric.end_time = time.time()
            metric.duration_ms = (metric.end_time - metric.start_time) * 1000

            # Check expectations
            expected_min = test_def.get('expected_min_rows')
            expected_max = test_def.get('expected_max_rows')

            if expected_min is not None and metric.rows_returned < expected_min:
                metric.status = "failed"
                metric.error_message = f"Too few rows: {metric.rows_returned} < {expected_min}"
            elif expected_max is not None and metric.rows_returned > expected_max:
                metric.status = "failed"
                metric.error_message = f"Too many rows: {metric.rows_returned} > {expected_max}"
            else:
                metric.status = "passed"
                metric.verification_passed = True

            print(f"  Duration: {metric.duration_ms:.2f}ms")
            print(f"  Status: {metric.status}")
            phase_extra.update({
                "duration_ms": metric.duration_ms,
                "rows_returned": metric.rows_returned,
                "rows_affected": metric.rows_affected,
                "verification_passed": metric.verification_passed,
            })
            if (
                phase_dir_value
                and self.engine == "postgresql"
                and "postgresql_explain" not in phase_extra
            ):
                phase_extra["postgresql_explain"] = self._capture_postgresql_explain(
                    sql,
                    Path(phase_dir_value),
                )
            self._finish_phase_diagnostics(
                phase_entry,
                status=metric.status,
                extra=phase_extra,
            )

        except Exception as e:
            metric.end_time = time.time()
            metric.duration_ms = (metric.end_time - metric.start_time) * 1000
            metric.status = "error"
            metric.error_message = str(e)
            print(f"  ERROR: {e}")
            traceback.print_exc()
            self._finish_phase_diagnostics(
                phase_entry,
                status="error",
                extra={
                    "duration_ms": metric.duration_ms,
                    "error_message": str(e),
                    "sql": sql,
                },
            )

        self.metrics.append(metric)
        return metric

    def run_all_tests(self, test_filter: Optional[str] = None):
        """Run all dialect-aware stress tests."""
        tests = get_tests_for_engine(self.engine)

        if test_filter:
            tests = {k: v for k, v in tests.items() if test_filter in k}

        print(f"\n{'='*60}")
        print(f"Running {len(tests)} stress tests ({self.engine} dialect)")
        print(f"{'='*60}")

        for i, (test_name, test_def) in enumerate(tests.items(), 1):
            print(f"\n[{i}/{len(tests)}] ", end="")
            self.run_test(test_name, test_def)

        print(f"\n{'='*60}")
        print(f"Stress tests complete")
        print(f"{'='*60}")

    def build_execution_lane_provenance(self):
        """Return the declared execution lane for this result bundle."""
        effective_mode = getattr(self, "effective_transaction_mode", "unknown")
        requested_mode = getattr(self, "requested_transaction_mode", effective_mode)
        if effective_mode == "normal_transactional":
            commit_grouping_policy = "explicit commit after each seed executemany batch"
        elif effective_mode == "autocommit":
            commit_grouping_policy = "autocommit lane with executemany batching"
        else:
            commit_grouping_policy = "non-transactional lane with executemany batching"

        return {
            "schema_version": "scratchbird_benchmarks.execution_lane.v3",
            "suite": "stress-tests",
            "lane_class": "common_portable_lane",
            "evidence_scope": "portable_engine_stress",
            "semantic_contract": (
                "generated-row seeding plus dialect-aware statement-and-transaction stress under "
                "the declared transaction mode and batch-commit policy; valid for the declared "
                "portable contract, not for reference-native ingest throughput claims"
            ),
            "claim_ceiling": (
                "portable stress lane only; no native-ingest speed claim and no firm planner "
                "claim beyond declared sample-plan capture are allowed from this bundle"
            ),
            "engine_name": self.engine,
            "engine_version": getattr(self, "engine_version", "unknown"),
            "driver_name": "stress-tests/dialect_stress_runner",
            "driver_version": "local",
            "storage_engine": "engine_default",
            "schema_artifact": "runner_defined_runtime_ddl",
            "index_artifact": "runner_defined_runtime_ddl",
            "encoding_collation_profile": {
                "encoding": "engine_default",
                "collation": "engine_default",
                "notes": (
                    "dialect stress runner uses engine-default encoding and collation unless "
                    "the selected engine or runtime DDL changes them"
                ),
            },
            "dataset_profile": {
                "generator": "dialect_stress_seed_generator",
                "seed": "table_shape_deterministic_runner_defined",
                "distribution": "runner_defined_mixed_workload_rows",
                "scale": "runner_defined_per_table_row_count",
            },
            "concurrency_profile": {
                "sessions": "single_connection_per_run",
                "clients": "single_client",
                "threads": "single_process_single_worker",
            },
            "cache_state": {
                "buffer_cache": "warm_after_seed_unpinned",
                "plan_cache": "engine_default",
                "filesystem_cache": "not_controlled",
            },
            "durability_profile": {
                "target": "statement_and_transaction_stress_under_declared_transaction_mode",
                "fsync_policy": "engine_default",
                "crash_recovery": "not_measured",
            },
            "measurement_policy": {
                "run_count": "single_pass_per_result_bundle",
                "warmup_policy": "implicit_seed_then_measure",
                "latency_statistics": "bundle_level_duration_only",
                "outlier_policy": "not_claimed",
            },
            "transaction_mode": effective_mode,
            "load_mechanism": "generated rows via adapter executemany batches",
            "ingest_sub_lane": "prepared_multi_row_batching",
            "batch_size": getattr(self, "current_load_batch_size", None),
            "commit_grouping_policy": commit_grouping_policy,
            "prepared_or_batch_behavior": "adapter executemany batching; no reference-native bulk loader lane",
            "statistics_refresh": "none_declared_for_stress_suite",
            "plan_capture_mode": "postgresql_sample_explain_only" if self.engine == "postgresql" else "none",
            "capability_waived": ["engine_native_ingest_lane"],
            "waived_claims": ["ingest_throughput"],
            "waiver_rationale": [
                "This portable stress lane does not exercise reference-native ingest paths."
            ],
            "degraded_lane_justification": "Stress suite is valid for statement and transaction pressure, not firm ingest throughput.",
            "notes": (
                f"Requested transaction mode={requested_mode}. "
                "Ingest throughput is explicitly waived in this portable stress lane."
            ),
        }

    def save_results(self):
        """Save test results to JSON file."""
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        results_file = self.output_dir / (
            f"stress_{self.engine}_{self.effective_transaction_mode}_{timestamp}.json"
        )
        execution_lane_provenance = self.build_execution_lane_provenance()

        results_data = {
            'metadata': {
                'engine': self.engine,
                'dialect': self.engine,
                'requested_transaction_mode': self.requested_transaction_mode,
                'transaction_mode': self.effective_transaction_mode,
                'host': self.host,
                'port': self.port,
                'database': self.database,
                'timestamp': timestamp,
                'execution_lane_provenance': execution_lane_provenance,
            },
            'data_loading': [
                {
                    'table_name': m.table_name,
                    'row_count': m.row_count,
                    'duration_ms': m.duration_ms,
                    'rows_per_second': m.rows_per_second,
                    'status': m.status,
                }
                for m in self.load_metrics
            ],
            'test_results': [
                {
                    'test_name': m.test_name,
                    'description': m.description,
                    'status': m.status,
                    'duration_ms': m.duration_ms,
                    'rows_returned': m.rows_returned,
                    'rows_affected': m.rows_affected,
                    'verification_passed': m.verification_passed,
                    'error_message': m.error_message,
                    'sql_executed': m.sql_executed[:200] if m.sql_executed else "",
                }
                for m in self.metrics
            ],
            'summary': {
                'total_tests': len(self.metrics),
                'passed': sum(1 for m in self.metrics if m.status == 'passed'),
                'failed': sum(1 for m in self.metrics if m.status == 'failed'),
                'errors': sum(1 for m in self.metrics if m.status == 'error'),
                'total_duration_ms': sum(m.duration_ms for m in self.metrics),
            }
        }

        results_file.write_text(json.dumps(results_data, indent=2))
        results_file.with_name(results_file.stem + ".lane.json").write_text(
            json.dumps(execution_lane_provenance, indent=2)
        )
        print(f"\nResults saved to: {results_file}")
        return results_file

    def print_summary(self):
        """Print summary of test results."""
        print(f"\n{'='*60}")
        print(f"STRESS TEST SUMMARY - {self.engine.upper()}")
        print(f"SQL Dialect: {self.engine}")
        print(f"{'='*60}")

        # Data loading summary
        print("\nData Loading:")
        total_rows = sum(m.row_count for m in self.load_metrics)
        total_time = sum(m.duration_ms for m in self.load_metrics)
        avg_rate = total_rows / (total_time / 1000) if total_time > 0 else 0

        for m in self.load_metrics:
            status_icon = "✓" if m.status == "success" else "✗"
            print(f"  {status_icon} {m.table_name}: {m.row_count:,} rows "
                  f"in {m.duration_ms/1000:.2f}s ({m.rows_per_second:,.0f} rows/s)")

        print(f"\n  Total: {total_rows:,} rows loaded in {total_time/1000:.2f}s "
              f"({avg_rate:,.0f} rows/s avg)")

        # Test results summary
        print("\nTest Results:")
        passed = sum(1 for m in self.metrics if m.status == 'passed')
        failed = sum(1 for m in self.metrics if m.status == 'failed')
        errors = sum(1 for m in self.metrics if m.status == 'error')

        for m in self.metrics:
            status_icon = "✓" if m.status == 'passed' else "✗"
            print(f"  {status_icon} {m.test_name}: {m.duration_ms:.2f}ms "
                  f"({m.rows_returned:,} rows)")

        total_duration = sum(m.duration_ms for m in self.metrics)
        print(f"\n  Total: {len(self.metrics)} tests, {passed} passed, "
              f"{failed} failed, {errors} errors")
        print(f"  Total time: {total_duration/1000:.2f}s")


def main():
    parser = argparse.ArgumentParser(description='Dialect-Aware Stress Test Runner')
    parser.add_argument('--engine', required=True,
                        choices=['firebird', 'mysql', 'postgresql', 'scratchbird'],
                        help='Database engine to test')
    parser.add_argument('--host', default='localhost',
                        help='Database host')
    parser.add_argument('--port', type=int,
                        help='Database port (default: engine default)')
    parser.add_argument('--database', default='benchmark',
                        help='Database name')
    parser.add_argument('--user', required=True,
                        help='Database user')
    parser.add_argument('--password', required=True,
                        help='Database password')
    parser.add_argument('--scale', default='small',
                        choices=['small', 'medium', 'large', 'huge'],
                        help='Data scale for stress tests')
    parser.add_argument('--output-dir', type=Path, default=Path('./results'),
                        help='Output directory for results')
    parser.add_argument('--test-filter', default=None,
                        help='Filter tests by name substring')
    parser.add_argument('--skip-data-load', action='store_true',
                        help='Skip data loading (use existing data)')
    parser.add_argument('--emit-load-sql', type=Path, default=None,
                        help='Write schema and load phase as a plain SQL script')
    parser.add_argument('--emit-load-sql-only', action='store_true',
                        help='Write the SQL load script and exit without running the benchmark')
    parser.add_argument('--load-batch-size', type=int, default=10000,
                        help='Requested row batch size for load/export operations')
    parser.add_argument('--transaction-mode', default='engine_default',
                        choices=[
                            'engine_default',
                            'normal_transactional',
                            'autocommit',
                            'no_transaction',
                            'always_in_transaction',
                            'autocommit_statement',
                        ],
                        help='Transaction behavior for the benchmark connection')
    parser.add_argument('--diagnostics-dir', type=Path, default=None,
                        help='Directory to store per-phase diagnostics and summaries')
    parser.add_argument('--capture-scratchbird-traces', action='store_true',
                        help='Capture ScratchBird trace files for each phase')
    parser.add_argument('--capture-postgresql-explain', action='store_true',
                        help='Capture PostgreSQL EXPLAIN ANALYZE JSON per diagnostic phase')

    args = parser.parse_args()

    # Set default ports
    if args.port is None:
        ports = {'firebird': 3050, 'mysql': 3306, 'postgresql': 5432}
        args.port = ports[args.engine]

    # Create runner
    runner = DialectStressTestRunner(
        engine=args.engine,
        host=args.host,
        port=args.port,
        database=args.database,
        user=args.user,
        password=args.password,
        output_dir=args.output_dir,
        transaction_mode=args.transaction_mode,
        diagnostics_dir=args.diagnostics_dir,
        capture_scratchbird_traces=args.capture_scratchbird_traces,
        capture_postgresql_explain=args.capture_postgresql_explain,
    )

    verification_ok = True
    dataset = generate_standard_dataset(args.scale)

    if args.emit_load_sql is not None:
        runner.export_load_sql(dataset, args.emit_load_sql, batch_size=args.load_batch_size)
        if args.emit_load_sql_only:
            return 0

    try:
        # Connect
        runner.connect()

        # Create schema and load data
        if not args.skip_data_load:
            runner.create_schema(dataset)
            runner.load_data(dataset, batch_size=args.load_batch_size)
            runner.create_workload_indexes()

            # Verify data integrity
            if not runner.verify_data(dataset):
                print("\nWARNING: Data verification failed!")
                verification_ok = False

        # Run stress tests
        runner.run_all_tests(args.test_filter)

        # Print summary
        runner.print_summary()

        # Save results
        results_file = runner.save_results()
        diagnostics_summary = runner.save_diagnostics_summary()
        if diagnostics_summary is not None:
            print(f"Diagnostics summary saved to: {diagnostics_summary}")

    finally:
        runner.disconnect()

    has_test_failures = any(metric.status in ("failed", "error") for metric in runner.metrics)
    return 1 if has_test_failures or not verification_ok else 0


if __name__ == '__main__':
    sys.exit(main())
