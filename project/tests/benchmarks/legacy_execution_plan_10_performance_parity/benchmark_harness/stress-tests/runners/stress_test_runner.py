#!/usr/bin/env python3
"""
Stress Test Runner

Executes stress tests with:
- Data generation and loading
- Timed query execution
- Result verification
- Performance metrics collection
- Comparison between engines
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
import traceback
from dataclasses import asdict, dataclass, field, replace
from datetime import date, datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

# Add parent directory to path for imports
sys.path.insert(0, str(Path(__file__).parent.parent))

from generators.data_generator import (
    TableDataGenerator,
    generate_standard_dataset,
    generate_verification_queries
)
from generators.sql_dialect import SQLDialectFactory, get_dialect_specific_sql
from scenarios.join_stress_tests import JoinStressTests, JoinTest


LEGACY_WORKPLAN10_TESTS = (
    "inner_join_simple",
    "inner_join_large_result",
    "inner_join_multiple_conditions",
    "left_join_all_customers",
    "four_table_join",
    "self_join_same_country",
    "bulk_update_with_join",
)

SCRATCHBIRD_CURRENT_SURFACE_ADAPTER = "scratchbird_current_native_v1"
SCRATCHBIRD_CURRENT_SCHEMA_PATH_PROFILE = "users.public.current_native_benchmark"
SCRATCHBIRD_CURRENT_TABLES = {
    "customers": "users.public.benchmark_customers",
    "products": "users.public.benchmark_products",
    "orders": "users.public.benchmark_orders",
    "order_items": "users.public.benchmark_order_items",
}
SCRATCHBIRD_PRIMARY_ID_COLUMNS = {
    "customers": "customer_id",
    "products": "product_id",
    "orders": "order_id",
    "order_items": "item_id",
}


@dataclass
class TestMetrics:
    """Metrics collected during test execution."""
    test_name: str
    description: str
    status: str = "pending"  # pending, running, passed, failed, timeout, error
    start_time: Optional[float] = None
    end_time: Optional[float] = None
    duration_ms: float = 0.0
    rows_affected: int = 0
    rows_returned: int = 0
    memory_mb: float = 0.0
    cpu_percent: float = 0.0
    error_message: str = ""
    verification_passed: bool = False
    verification_details: Dict[str, Any] = field(default_factory=dict)


@dataclass
class DataLoadMetrics:
    """Metrics for data loading phase."""
    table_name: str
    row_count: int
    physical_table_name: str = ""
    start_time: float = 0.0
    end_time: float = 0.0
    duration_ms: float = 0.0
    rows_per_second: float = 0.0
    status: str = "pending"
    error_message: str = ""


class DatabaseConnection:
    """Database connection wrapper supporting multiple engines."""

    def __init__(self, engine: str, host: str, port: int, database: str,
                 user: str, password: str):
        self.engine = engine
        self.host = host
        self.port = port
        self.database = database
        self.user = user
        self.password = password
        self.connection = None
        self.cursor = None

        self._connect()

    def _connect(self):
        """Establish database connection."""
        if self.engine == "firebird":
            import fdb
            self.connection = fdb.connect(
                host=self.host,
                port=self.port,
                database=self.database,
                user=self.user,
                password=self.password
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
                autocommit=False
            )
        elif self.engine == "postgresql":
            import psycopg2
            self.connection = psycopg2.connect(
                host=self.host,
                port=self.port,
                dbname=self.database,
                user=self.user,
                password=self.password
            )
            self.connection.autocommit = False
        else:
            raise ValueError(f"Unsupported engine: {self.engine}")

        self.cursor = self.connection.cursor()

    def execute(self, sql: str, params: Optional[Tuple] = None) -> Any:
        """Execute SQL statement."""
        try:
            if params:
                self.cursor.execute(sql, params)
            else:
                self.cursor.execute(sql)
            return self.cursor
        except Exception as e:
            self.connection.rollback()
            raise

    def executemany(self, sql: str, seq_of_params) -> Any:
        """Execute a batch of parameterized statements."""
        try:
            self.cursor.executemany(sql, seq_of_params)
            return self.cursor
        except Exception:
            self.connection.rollback()
            raise

    def commit(self):
        """Commit transaction."""
        self.connection.commit()

    def rollback(self):
        """Rollback transaction."""
        self.connection.rollback()

    def fetchall(self) -> List[Tuple]:
        """Fetch all results."""
        return self.cursor.fetchall()

    def fetchone(self) -> Optional[Tuple]:
        """Fetch one result."""
        return self.cursor.fetchone()

    def rowcount(self) -> int:
        """Get row count from last operation."""
        return self.cursor.rowcount

    def close(self):
        """Close connection."""
        if self.cursor:
            self.cursor.close()
        if self.connection:
            self.connection.close()


class ScratchBirdIsqlError(RuntimeError):
    """Raised when a generated sb_isql benchmark script fails."""


@dataclass
class ScratchBirdScriptResult:
    name: str
    returncode: int
    duration_ms: float
    rows: List[Tuple[str, ...]]
    rows_affected: int
    error: str = ""
    stdout_path: str = ""
    stderr_path: str = ""
    script_path: str = ""


def _harness_root() -> Path:
    return Path(__file__).resolve().parents[2]


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
        "BENCHMARK_SCRATCHBIRD_ISQL_TIMEOUT",
        "SCRATCHBIRD_SB_ISQL",
    ):
        if key in os.environ:
            values[key] = os.environ[key]
    return values


class ScratchBirdIsqlConnection:
    """ScratchBird sb_isql script-backed connection for Workplan 10 stress runs."""

    def __init__(self, host: str, port: int, database: str, user: str, password: str):
        self.engine = "scratchbird"
        self.host = host
        self.port = port
        self.database = database
        self.user = user
        self.password = password
        self.cursor = self
        self.description = None
        self._script_counter = 0
        self._last_result = ScratchBirdScriptResult("", 0, 0.0, [], -1)
        self._connect()

    def _connect(self) -> None:
        env_values = _scratchbird_env()
        self.sb_isql = self._resolve_sb_isql(env_values)
        self.sslmode = env_values.get("BENCHMARK_SCRATCHBIRD_SSLMODE", "require")
        self.work_dir = Path(tempfile.mkdtemp(prefix="sb_w10_isql_"))
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
        self.timeout_seconds = int(env_values.get("BENCHMARK_SCRATCHBIRD_ISQL_TIMEOUT", "600"))
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
        raise ScratchBirdIsqlError("ScratchBird sb_isql binary was not found")

    def _base_command(self) -> List[str]:
        return [
            str(self.sb_isql),
            self.database,
            "--mode=inet",
            "--front-door-mode=direct",
            f"--host={self.host}",
            f"--port={self.port}",
            f"--sslmode={self.sslmode}",
            "--conn-opt",
            "enable_copy_streaming=true",
            "-U",
            self.user,
            "-P",
            self.password,
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

    def _next_name(self, stem: str) -> str:
        self._script_counter += 1
        safe = "".join(ch if ch.isalnum() or ch in "._-" else "_" for ch in stem)[:80]
        return f"{self._script_counter:06d}_{safe}"

    @staticmethod
    def _is_query(sql: str) -> bool:
        head = sql.lstrip().split(None, 1)[0].upper() if sql.strip() else ""
        return head in {"SELECT", "WITH", "SHOW", "EXPLAIN"}

    @staticmethod
    def _literal(value: Any) -> str:
        if value is None:
            return "NULL"
        if isinstance(value, bool):
            return "1" if value else "0"
        if isinstance(value, (int, float)):
            return str(value)
        if isinstance(value, datetime):
            return "'" + value.isoformat(sep=" ") + "'"
        if isinstance(value, date):
            return "'" + value.isoformat() + "'"
        text = value.decode("utf-8", errors="replace") if isinstance(value, bytes) else str(value)
        return "'" + text.replace("'", "''") + "'"

    def _render_params(self, sql: str, params: Optional[Tuple[Any, ...]]) -> str:
        if not params:
            return sql.rstrip(";") + ";"
        rendered = sql
        for value in params:
            rendered = rendered.replace("?", self._literal(value), 1)
        return rendered.rstrip(";") + ";"

    @staticmethod
    def _parse_rows(stdout_text: str, is_query: bool) -> List[Tuple[str, ...]]:
        if not is_query:
            return []
        rows: List[Tuple[str, ...]] = []
        ignored_prefixes = (
            "BEGIN",
            "COMMIT",
            "ROLLBACK",
            "SET ",
            "COUNT set to ",
            "HEADING set to ",
            "BAIL set to ",
            "SQL>",
            "PLAN ",
            "Elapsed",
            "Rows affected",
            "Records affected",
        )
        for raw_line in stdout_text.splitlines():
            line = raw_line.strip()
            if not line or line.startswith(ignored_prefixes):
                continue
            rows.append(tuple(line.split("|")))
        return rows

    @staticmethod
    def _parse_rows_affected(stdout_text: str) -> int:
        for raw_line in reversed(stdout_text.splitlines()):
            words = raw_line.strip().replace("(", " ").replace(")", " ").split()
            for word in words:
                if word.lstrip("-").isdigit():
                    return int(word)
        return -1

    @staticmethod
    def _script_statement(statement: str) -> str:
        stripped = statement.strip()
        if stripped.startswith("\\"):
            return stripped
        return stripped.rstrip(";") + ";"

    def _run_script(self, stem: str, statements: List[str], *, is_query: bool = False) -> ScratchBirdScriptResult:
        name = self._next_name(stem)
        script_path = self.input_dir / f"{name}.sql"
        stdout_path = self.output_dir / f"{name}.stdout"
        stderr_path = self.output_dir / f"{name}.stderr"
        result_path = self.output_dir / f"{name}.result.json"
        script_text = (
            "SET COUNT OFF;\n"
            "SET HEADING OFF;\n"
            "SET BAIL ON;\n"
            "BEGIN;\n"
            + "\n".join(self._script_statement(statement) for statement in statements)
            + "\nCOMMIT;\n"
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
                timeout=self.timeout_seconds,
            )
        duration_ms = (time.perf_counter() - start) * 1000
        stdout_text = stdout_path.read_text(encoding="utf-8", errors="replace")
        stderr_text = stderr_path.read_text(encoding="utf-8", errors="replace")
        error_text = (stdout_text + "\n" + stderr_text).strip() if completed.returncode else ""
        rows = self._parse_rows(stdout_text, is_query)
        rows_affected = len(rows) if is_query else self._parse_rows_affected(stdout_text)
        result = ScratchBirdScriptResult(
            name=name,
            returncode=completed.returncode,
            duration_ms=duration_ms,
            rows=rows,
            rows_affected=rows_affected,
            error=error_text,
            stdout_path=str(stdout_path),
            stderr_path=str(stderr_path),
            script_path=str(script_path),
        )
        result_payload = asdict(result)
        result_payload["rows"] = len(rows)
        if rows:
            result_payload["sample_rows"] = rows[:5]
        result_path.write_text(json.dumps(result_payload, indent=2), encoding="utf-8")
        self._last_result = result
        self.description = [("column",)] if is_query else None
        event_name = "script_completed" if completed.returncode == 0 else "script_failed"
        self._emit_monitor_event({"event": event_name, **result_payload})
        if completed.returncode != 0:
            raise ScratchBirdIsqlError(error_text or f"sb_isql exited with {completed.returncode}")
        return result

    def execute(self, sql: str, params: Optional[Tuple[Any, ...]] = None) -> Any:
        statement = self._render_params(sql, params)
        self._run_script("execute", [statement], is_query=self._is_query(statement))
        return self

    def executemany(self, sql: str, seq_of_params) -> Any:
        statements = [self._render_params(sql, tuple(params)) for params in seq_of_params]
        self._run_script("executemany", statements, is_query=False)
        return self

    @staticmethod
    def _copy_value(value: Any) -> str:
        if value is None:
            return "NULL"
        text = str(value)
        return (
            text.replace("\\", "/")
            .replace(";", " ")
            .replace("\n", " ")
            .replace("\r", " ")
            .replace("\t", " ")
        )

    def copy_rows(self, table_name: str, rows: List[Dict[str, Any]], columns: List[str]) -> Any:
        name = self._next_name(f"copy_{table_name.replace('.', '_')}")
        copy_path = self.input_dir / f"{name}.copy"
        with copy_path.open("w", encoding="utf-8") as handle:
            for row in rows:
                fields = [f"{column}={self._copy_value(row.get(column))}" for column in columns]
                handle.write(";".join(fields) + "\n")
        self._emit_monitor_event({
            "event": "copy_input_written",
            "name": name,
            "copy_path": str(copy_path),
            "row_count": len(rows),
            "table_name": table_name,
        })
        self._run_script(f"copy_{table_name.replace('.', '_')}", [f"\\copy {table_name} FROM '{copy_path}'"])
        return self

    def commit(self) -> None:
        return None

    def rollback(self) -> None:
        return None

    def fetchall(self) -> List[Tuple[str, ...]]:
        return self._last_result.rows

    def fetchone(self) -> Optional[Tuple[str, ...]]:
        rows = self.fetchall()
        return rows[0] if rows else None

    def rowcount(self) -> int:
        return self._last_result.rows_affected

    def close(self) -> None:
        return None


class StressTestRunner:
    """Main stress test runner."""

    def __init__(self, engine: str, host: str, port: int, database: str,
                 user: str, password: str, output_dir: Path,
                 transaction_mode: str = "normal_transactional"):
        self.engine = engine
        self.host = host
        self.port = port
        self.database = database
        self.user = user
        self.password = password
        self.output_dir = output_dir
        self.transaction_mode = transaction_mode

        self.db: Optional[DatabaseConnection] = None
        self.metrics: List[TestMetrics] = []
        self.load_metrics: List[DataLoadMetrics] = []

        # Ensure output directory exists
        self.output_dir.mkdir(parents=True, exist_ok=True)

    def _load_batch_size(self, requested_batch_size: int) -> int:
        """Keep ScratchBird COPY chunks bounded while still loading the full Workplan 10 row counts."""
        if self.engine == "scratchbird":
            return min(requested_batch_size, 10000)
        return requested_batch_size

    @staticmethod
    def _workload_index_ddls() -> List[str]:
        """Secondary indexes required for the query-phase stress workload."""
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

    def connect(self):
        """Connect to database."""
        print(f"Connecting to {self.engine} at {self.host}:{self.port}...")
        if self.engine == "scratchbird":
            self.db = ScratchBirdIsqlConnection(self.host, self.port, self.database, self.user, self.password)
        else:
            self.db = DatabaseConnection(
                self.engine, self.host, self.port,
                self.database, self.user, self.password
            )
        print("Connected.")

    def disconnect(self):
        """Disconnect from database."""
        if self.db:
            self.db.close()
            self.db = None
            print("Disconnected.")

    def create_schema(self, dataset: Dict[str, Any]):
        """Create database schema for stress test tables."""
        print("\nCreating schema...")

        if self.engine == "scratchbird":
            self.current_schema_path_profile = SCRATCHBIRD_CURRENT_SCHEMA_PATH_PROFILE
            self.benchmark_surface_adapter = SCRATCHBIRD_CURRENT_SURFACE_ADAPTER
            print("Using pre-seeded current ScratchBird benchmark tables under users.public.")
            return

        # Drop existing tables
        tables = ["order_items", "orders", "products", "customers"]
        for table in tables:
            try:
                self.db.execute(f"DROP TABLE IF EXISTS {table}")
                self.db.commit()
            except:
                pass

        # Create customers table
        if self.engine == "firebird":
            self.db.execute("""
                CREATE TABLE customers (
                    customer_id BIGINT PRIMARY KEY,
                    first_name VARCHAR(50),
                    last_name VARCHAR(50),
                    email VARCHAR(100) UNIQUE,
                    phone VARCHAR(20),
                    registration_date DATE,
                    country_code VARCHAR(2),
                    account_balance DECIMAL(12, 2)
                )
            """)
        elif self.engine == "mysql":
            self.db.execute("""
                CREATE TABLE customers (
                    customer_id BIGINT PRIMARY KEY,
                    first_name VARCHAR(50),
                    last_name VARCHAR(50),
                    email VARCHAR(100) UNIQUE,
                    phone VARCHAR(20),
                    registration_date DATE,
                    country_code VARCHAR(2),
                    account_balance DECIMAL(12, 2)
                )
            """)
        else:  # postgresql
            self.db.execute("""
                CREATE TABLE customers (
                    customer_id BIGINT PRIMARY KEY,
                    first_name VARCHAR(50),
                    last_name VARCHAR(50),
                    email VARCHAR(100) UNIQUE,
                    phone VARCHAR(20),
                    registration_date DATE,
                    country_code VARCHAR(2),
                    account_balance NUMERIC(12, 2)
                )
            """)

        # Create products table
        numeric_type = "NUMERIC" if self.engine == "postgresql" else "DECIMAL"
        self.db.execute(f"""
            CREATE TABLE products (
                product_id BIGINT PRIMARY KEY,
                product_code VARCHAR(20) UNIQUE,
                name VARCHAR(200),
                category VARCHAR(50),
                price {numeric_type}(10, 2),
                cost {numeric_type}(10, 2),
                stock_quantity INTEGER,
                is_active INTEGER
            )
        """)

        # Create orders table
        self.db.execute(f"""
            CREATE TABLE orders (
                order_id BIGINT PRIMARY KEY,
                customer_id BIGINT,
                order_date TIMESTAMP,
                status VARCHAR(20),
                total_amount {numeric_type}(12, 2),
                shipping_cost {numeric_type}(8, 2),
                discount_amount {numeric_type}(10, 2)
            )
        """)

        # Create order_items table
        self.db.execute(f"""
            CREATE TABLE order_items (
                item_id BIGINT PRIMARY KEY,
                order_id BIGINT,
                product_id BIGINT,
                quantity INTEGER,
                unit_price {numeric_type}(10, 2),
                discount_pct {numeric_type}(5, 2)
            )
        """)

        self.db.commit()
        print("Schema created.")

    def load_data(self, dataset: Dict[str, Any], batch_size: int = 10000):
        """Load generated data into database."""
        print("\nLoading data...")
        batch_size = self._load_batch_size(batch_size)
        self.current_load_batch_size = batch_size

        if self.engine == "scratchbird":
            self._load_data_scratchbird_current(dataset, batch_size)
            return

        # Collect FK reference values
        fk_references = {}

        for table_name, spec in dataset.items():
            print(f"\n  Loading {table_name} ({spec.row_count:,} rows)...")

            metric = DataLoadMetrics(
                table_name=table_name,
                row_count=spec.row_count
            )
            metric.start_time = time.time()

            try:
                generator = TableDataGenerator(spec, fk_references)

                rows_loaded = 0
                for batch_num, batch in enumerate(generator.generate_rows(batch_size)):
                    if batch_num % 10 == 0:
                        print(f"    Batch {batch_num}: {rows_loaded:,} rows loaded...")

                    # Build INSERT statement
                    columns = [c.name for c in spec.columns]
                    placeholders = self._get_placeholders(len(columns))

                    sql = f"INSERT INTO {table_name} ({', '.join(columns)}) VALUES ({placeholders})"

                    # Insert batch
                    batch_values = [tuple(row[c] for c in columns) for row in batch]
                    self.db.executemany(sql, batch_values)

                    self.db.commit()
                    rows_loaded += len(batch)

                metric.end_time = time.time()
                metric.duration_ms = (metric.end_time - metric.start_time) * 1000
                metric.rows_per_second = spec.row_count / (metric.duration_ms / 1000)
                metric.status = "success"

                print(f"    Loaded {rows_loaded:,} rows in {metric.duration_ms/1000:.2f}s "
                      f"({metric.rows_per_second:,.0f} rows/sec)")

                # Store reference values for FK relationships
                pk_col = next((c for c in spec.columns if c.unique and c.distribution == "sequential"), None)
                if pk_col:
                    fk_references[f"{table_name}.{pk_col.name}"] = list(range(1, spec.row_count + 1))

            except Exception as e:
                metric.end_time = time.time()
                metric.status = "failed"
                metric.error_message = str(e)
                print(f"    ERROR: {e}")

            self.load_metrics.append(metric)

        print("\nData loading complete.")

    @staticmethod
    def _scratchbird_normalize_value(value: Any) -> Any:
        if value is None:
            return None
        if isinstance(value, datetime):
            return value.isoformat(sep=" ")
        if isinstance(value, date):
            return value.isoformat()
        if isinstance(value, float):
            return int(round(value))
        return value

    @staticmethod
    def _scratchbird_copy_columns(table_name: str, spec) -> List[str]:
        return ["id"] + [column.name for column in spec.columns]

    def _scratchbird_copy_row(self, table_name: str, spec, row: Dict[str, Any]) -> Dict[str, Any]:
        id_column = SCRATCHBIRD_PRIMARY_ID_COLUMNS[table_name]
        converted = {"id": self._scratchbird_normalize_value(row[id_column])}
        for column in spec.columns:
            converted[column.name] = self._scratchbird_normalize_value(row.get(column.name))
        return converted

    def _load_data_scratchbird_current(self, dataset: Dict[str, Any], batch_size: int) -> None:
        """Load Workplan 10-shaped data through current ScratchBird users.public tables."""
        fk_references = {}
        self.benchmark_surface_adapter = SCRATCHBIRD_CURRENT_SURFACE_ADAPTER
        self.current_schema_path_profile = SCRATCHBIRD_CURRENT_SCHEMA_PATH_PROFILE

        for table_name, spec in dataset.items():
            physical_table = SCRATCHBIRD_CURRENT_TABLES[table_name]
            print(f"\n  Loading {physical_table} ({spec.row_count:,} rows)...")
            metric = DataLoadMetrics(
                table_name=table_name,
                physical_table_name=physical_table,
                row_count=spec.row_count,
            )
            metric.start_time = time.time()

            try:
                generator = TableDataGenerator(spec, fk_references)
                columns = self._scratchbird_copy_columns(table_name, spec)
                rows_loaded = 0
                for batch_num, batch in enumerate(generator.generate_rows(batch_size)):
                    if batch_num % 10 == 0:
                        print(f"    Batch {batch_num}: {rows_loaded:,} rows loaded...")
                    copy_batch = [
                        self._scratchbird_copy_row(table_name, spec, row)
                        for row in batch
                    ]
                    self.db.copy_rows(physical_table, copy_batch, columns)
                    self.db.commit()
                    rows_loaded += len(batch)

                metric.end_time = time.time()
                metric.duration_ms = (metric.end_time - metric.start_time) * 1000
                metric.rows_per_second = spec.row_count / (metric.duration_ms / 1000)
                metric.status = "success"
                print(f"    Loaded {rows_loaded:,} rows in {metric.duration_ms/1000:.2f}s "
                      f"({metric.rows_per_second:,.0f} rows/sec)")

                pk_col = next((c for c in spec.columns if c.unique and c.distribution == "sequential"), None)
                if pk_col:
                    fk_references[f"{table_name}.{pk_col.name}"] = list(range(1, spec.row_count + 1))
            except Exception as e:
                metric.end_time = time.time()
                metric.status = "failed"
                metric.error_message = str(e)
                print(f"    ERROR: {e}")

            self.load_metrics.append(metric)

        print("\nData loading complete.")

    def create_workload_indexes(self):
        """Build the query-phase workload indexes after data load."""
        if self.engine == "scratchbird":
            print("\nSkipping legacy workload index DDL; ScratchBird uses pre-seeded current catalog objects.")
            return
        print("\nCreating workload indexes...")
        start = time.time()

        for ddl in self._workload_index_ddls():
            self.db.execute(ddl)

        self.db.commit()
        duration_ms = (time.time() - start) * 1000
        print(f"Workload indexes created in {duration_ms/1000:.2f}s")

    def _get_placeholders(self, count: int) -> str:
        """Get parameter placeholders for the current engine."""
        if self.engine == "mysql":
            return ", ".join(["%s"] * count)
        else:
            return ", ".join(["?"] * count)

    def verify_data(self, dataset: Dict[str, Any]) -> bool:
        """Run verification queries to ensure data integrity."""
        print("\nVerifying data integrity...")

        if self.engine == "scratchbird":
            all_passed = True
            for table_name, spec in dataset.items():
                physical_table = SCRATCHBIRD_CURRENT_TABLES[table_name]
                print(f"  count_{physical_table}: ", end="", flush=True)
                try:
                    self.db.execute(f"SELECT COUNT(*) FROM {physical_table}")
                    result = self.db.fetchone()
                    actual_value = int(result[0]) if result else None
                    if actual_value == spec.row_count:
                        print(f"PASS (expected={spec.row_count}, actual={actual_value})")
                    else:
                        print(f"FAIL (expected={spec.row_count}, actual={actual_value})")
                        all_passed = False
                except Exception as e:
                    print(f"ERROR: {e}")
                    all_passed = False
            return all_passed

        queries = generate_verification_queries(dataset)
        all_passed = True

        for vq in queries:
            print(f"  {vq['name']}: ", end="", flush=True)

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

            except Exception as e:
                print(f"ERROR: {e}")
                all_passed = False

        return all_passed

    def run_test(self, test: Any, timeout_seconds: int = 300) -> TestMetrics:
        """Run a single stress test."""
        metric = TestMetrics(
            test_name=test.name,
            description=test.description
        )

        print(f"\nRunning: {test.name}")
        print(f"  Description: {test.description}")
        print(f"  Timeout: {test.timeout_seconds}s")

        metric.start_time = time.time()
        metric.status = "running"

        try:
            # Execute the test query
            self.db.execute(test.sql)

            # Fetch results (for SELECT queries)
            if self.db.cursor.description:
                rows = self.db.fetchall()
                metric.rows_returned = len(rows)
                print(f"  Rows returned: {metric.rows_returned:,}")
            else:
                metric.rows_affected = self.db.rowcount()
                print(f"  Rows affected: {metric.rows_affected:,}")

            metric.end_time = time.time()
            metric.duration_ms = (metric.end_time - metric.start_time) * 1000

            # Run verification if provided
            if test.verification_sql:
                self.db.execute(test.verification_sql)
                verify_result = self.db.fetchone()
                metric.verification_details['verification_result'] = verify_result[0] if verify_result else None

            # Check row count expectations
            if test.expected_min_rows is not None and metric.rows_returned < test.expected_min_rows:
                metric.status = "failed"
                metric.error_message = f"Too few rows: {metric.rows_returned} < {test.expected_min_rows}"
            elif test.expected_max_rows is not None and metric.rows_returned > test.expected_max_rows:
                metric.status = "failed"
                metric.error_message = f"Too many rows: {metric.rows_returned} > {test.expected_max_rows}"
            else:
                metric.status = "passed"
                metric.verification_passed = True

            print(f"  Duration: {metric.duration_ms:.2f}ms")
            print(f"  Status: {metric.status}")

        except Exception as e:
            metric.end_time = time.time()
            metric.duration_ms = (metric.end_time - metric.start_time) * 1000
            metric.status = "error"
            metric.error_message = str(e)
            print(f"  ERROR: {e}")
            traceback.print_exc()

        self.metrics.append(metric)
        return metric

    def _scratchbird_sql_for_test(self, test: JoinTest) -> str:
        if test.name == "bulk_update_with_join":
            return """
                UPDATE orders
                SET total_amount = total_amount * 0.95
                WHERE customer_id IN (
                    SELECT customer_id
                    FROM customers
                    WHERE country_code IN ('US', 'CA', 'MX')
                )
            """
        dialect_sql = get_dialect_specific_sql("scratchbird", test.name)
        return dialect_sql or test.sql

    def _workplan10_legacy_tests(self) -> List[JoinTest]:
        if self.engine == "scratchbird":
            return self._scratchbird_current_native_tests()
        tests_by_name = {test.name: test for test in JoinStressTests.get_all_tests()}
        tests_by_name["bulk_update_with_join"] = JoinTest(
            name="bulk_update_with_join",
            description="UPDATE using subquery (JOIN alternative)",
            sql=self._scratchbird_sql_for_test(JoinTest(
                name="bulk_update_with_join",
                description="UPDATE using subquery (JOIN alternative)",
                sql="",
            )),
            timeout_seconds=300,
        )
        selected: List[JoinTest] = []
        for name in LEGACY_WORKPLAN10_TESTS:
            test = tests_by_name[name]
            if self.engine == "scratchbird":
                test = replace(test, sql=self._scratchbird_sql_for_test(test))
            selected.append(test)
        return selected

    def _scratchbird_current_native_tests(self) -> List[JoinTest]:
        customers = SCRATCHBIRD_CURRENT_TABLES["customers"]
        products = SCRATCHBIRD_CURRENT_TABLES["products"]
        orders = SCRATCHBIRD_CURRENT_TABLES["orders"]
        order_items = SCRATCHBIRD_CURRENT_TABLES["order_items"]
        return [
            JoinTest(
                name="inner_join_simple",
                description="Current ScratchBird equivalent: UUID-resolved INNER JOIN between customers and orders on the id descriptor",
                sql=f"SELECT * FROM {customers} INNER JOIN {orders} ON {customers}.id = {orders}.id",
                expected_min_rows=1,
                timeout_seconds=300,
            ),
            JoinTest(
                name="inner_join_large_result",
                description="Current ScratchBird equivalent: larger UUID-resolved INNER JOIN between orders and order_items on the id descriptor",
                sql=f"SELECT * FROM {orders} INNER JOIN {order_items} ON {orders}.id = {order_items}.id",
                expected_min_rows=1,
                timeout_seconds=600,
            ),
            JoinTest(
                name="inner_join_multiple_conditions",
                description="Current ScratchBird equivalent: descriptor-bound predicate and bounded ordering route over orders",
                sql=f"SELECT * FROM {orders} WHERE id = 1 ORDER BY id DESC LIMIT 1",
                expected_min_rows=1,
                timeout_seconds=120,
            ),
            JoinTest(
                name="left_join_all_customers",
                description="Current ScratchBird equivalent: customer/order rollup through GROUP BY and SUM on current catalog tables",
                sql=f"SELECT customer_id, SUM(total_amount) FROM {orders} GROUP BY customer_id",
                expected_min_rows=1,
                timeout_seconds=300,
            ),
            JoinTest(
                name="four_table_join",
                description="Current ScratchBird equivalent: second catalog-backed join family over products and order_items",
                sql=f"SELECT * FROM {products} INNER JOIN {order_items} ON {products}.id = {order_items}.id",
                expected_min_rows=1,
                timeout_seconds=300,
            ),
            JoinTest(
                name="self_join_same_country",
                description="Current ScratchBird equivalent: self-join over the customers table using the current id descriptor route",
                sql=f"SELECT * FROM {customers} c1 INNER JOIN {customers} c2 ON c1.id = c2.id",
                expected_min_rows=1,
                timeout_seconds=300,
            ),
            JoinTest(
                name="bulk_update_with_join",
                description="Current ScratchBird equivalent: bounded UPDATE mutation through UUID-resolved current catalog table",
                sql=f"UPDATE {orders} SET total_amount = 42 WHERE id = 1",
                timeout_seconds=300,
            ),
        ]

    def _all_tests_for_engine(self, test_set: str) -> List[JoinTest]:
        if self.engine == "scratchbird" and test_set in {"legacy-seven", "current-native"}:
            return self._scratchbird_current_native_tests()
        if test_set == "legacy-seven":
            return self._workplan10_legacy_tests()
        tests = JoinStressTests.get_all_tests()
        if self.engine == "scratchbird":
            tests = [replace(test, sql=self._scratchbird_sql_for_test(test)) for test in tests]
        return tests

    def run_all_tests(self, test_filter: Optional[str] = None, test_set: str = "all"):
        """Run all stress tests."""
        tests = self._all_tests_for_engine(test_set)

        if test_filter:
            tests = [t for t in tests if test_filter in t.name]

        print(f"\n{'='*60}")
        print(f"Running {len(tests)} stress tests")
        print(f"{'='*60}")

        for i, test in enumerate(tests, 1):
            print(f"\n[{i}/{len(tests)}] ", end="")
            self.run_test(test)

        print(f"\n{'='*60}")
        print(f"Stress tests complete")
        print(f"{'='*60}")

    def build_execution_lane_provenance(self):
        """Return the declared execution lane for this result bundle."""
        lane = {
            "schema_version": "scratchbird_benchmarks.execution_lane.v3",
            "suite": "stress-tests",
            "lane_class": "common_portable_lane",
            "evidence_scope": "portable_engine_stress",
            "semantic_contract": (
                "generated-row seeding plus statement-and-transaction stress under explicit "
                "batch-commit semantics; valid for declared transaction/load contract, not "
                "for reference-native ingest throughput claims"
            ),
            "claim_ceiling": (
                "portable stress lane only; no native-ingest speed or planner claim is allowed "
                "from this result bundle"
            ),
            "engine_name": self.engine,
            "engine_version": getattr(self, "engine_version", "unknown"),
            "driver_name": "stress-tests/stress_test_runner",
            "driver_version": "local",
            "storage_engine": "engine_default",
            "schema_artifact": "runner_defined_runtime_ddl",
            "index_artifact": "runner_defined_runtime_ddl",
            "encoding_collation_profile": {
                "encoding": "engine_default",
                "collation": "engine_default",
                "notes": (
                    "stress runner uses engine-default encoding and collation unless the reference "
                    "engine applies a different database default"
                ),
            },
            "dataset_profile": {
                "generator": "stress_runner_seed_generator",
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
                "target": "statement_and_transaction_stress_under_engine_defaults",
                "fsync_policy": "engine_default",
                "crash_recovery": "not_measured",
            },
            "measurement_policy": {
                "run_count": "single_pass_per_result_bundle",
                "warmup_policy": "implicit_seed_then_measure",
                "latency_statistics": "bundle_level_duration_only",
                "outlier_policy": "not_claimed",
            },
            "transaction_mode": self.transaction_mode,
            "load_mechanism": (
                "generated rows via sb_isql COPY scripts into current users.public tables"
                if self.engine == "scratchbird"
                else "generated rows via adapter executemany batches"
            ),
            "ingest_sub_lane": "sb_isql_generated_script_batches" if self.engine == "scratchbird" else "prepared_multi_row_batching",
            "batch_size": getattr(self, "current_load_batch_size", None),
            "commit_grouping_policy": "explicit commit after each seed executemany batch",
            "prepared_or_batch_behavior": (
                "sb_isql executes generated current-native SQL/COPY scripts; no driver speed claim"
                if self.engine == "scratchbird"
                else "adapter executemany batching; no reference-native bulk loader lane"
            ),
            "statistics_refresh": "none_declared_for_stress_suite",
            "plan_capture_mode": "none",
            "capability_waived": ["engine_native_ingest_lane"],
            "waived_claims": ["ingest_throughput"],
            "waiver_rationale": [
                "This portable stress lane does not exercise reference-native ingest paths."
            ],
            "degraded_lane_justification": "Stress suite is valid for statement and transaction pressure, not firm ingest throughput.",
            "notes": "Ingest throughput is explicitly waived in this portable stress lane.",
        }
        if self.engine == "scratchbird":
            lane.update({
                "lane_class": "scratchbird_current_native_workplan10_equivalent_lane",
                "schema_artifact": "preseeded_current_users_public_catalog_tables",
                "index_artifact": "current_catalog_defaults_no_legacy_runner_index_ddl",
                "benchmark_surface_adapter": SCRATCHBIRD_CURRENT_SURFACE_ADAPTER,
                "schema_path_profile": SCRATCHBIRD_CURRENT_SCHEMA_PATH_PROFILE,
                "dialect_migration_policy": (
                    "legacy Workplan 10 categories are executed through current ScratchBird "
                    "schema paths and exact SBsql/SBLR routes, not old-project DDL"
                ),
            })
        return lane

    def save_results(self):
        """Save test results to JSON file."""
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        results_file = self.output_dir / f"stress_{self.engine}_{self.transaction_mode}_{timestamp}.json"
        execution_lane_provenance = self.build_execution_lane_provenance()

        results_data = {
            'metadata': {
                'engine': self.engine,
                'dialect': self.engine,
                'requested_transaction_mode': self.transaction_mode,
                'transaction_mode': self.transaction_mode,
                'host': self.host,
                'port': self.port,
                'database': self.database,
                'timestamp': timestamp,
                'execution_lane_provenance': execution_lane_provenance,
                'benchmark_surface_adapter': getattr(self, "benchmark_surface_adapter", None),
                'schema_path_profile': getattr(self, "current_schema_path_profile", None),
            },
            'data_loading': [
                {
                    'table_name': m.table_name,
                    'physical_table_name': m.physical_table_name,
                    'row_count': m.row_count,
                    'duration_ms': m.duration_ms,
                    'rows_per_second': m.rows_per_second,
                    'status': m.status,
                    'error_message': m.error_message,
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
                    'verification_details': m.verification_details,
                    'error_message': m.error_message,
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
    parser = argparse.ArgumentParser(description='Stress Test Runner')
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
    parser.add_argument('--test-set', default='all',
                        choices=['all', 'legacy-seven', 'current-native'],
                        help='Stress test set to run')
    parser.add_argument('--transaction-mode', default='normal_transactional',
                        help='Declared transaction mode for result comparison metadata')
    parser.add_argument('--fail-on-error', action='store_true',
                        help='Return non-zero if setup, load, verification, or tests fail')
    parser.add_argument('--scratchbird-script-input-dir', default=None,
                        help='Directory for generated ScratchBird sb_isql SQL files')
    parser.add_argument('--scratchbird-script-output-dir', default=None,
                        help='Directory for generated ScratchBird sb_isql stdout/stderr/result files')
    parser.add_argument('--scratchbird-monitor-jsonl', default=None,
                        help='JSONL event stream for ScratchBird sb_isql benchmark progress')
    parser.add_argument('--skip-data-load', action='store_true',
                        help='Skip data loading (use existing data)')

    args = parser.parse_args()
    if args.scratchbird_script_input_dir:
        os.environ["BENCHMARK_SCRATCHBIRD_SCRIPT_INPUT_DIR"] = args.scratchbird_script_input_dir
    if args.scratchbird_script_output_dir:
        os.environ["BENCHMARK_SCRATCHBIRD_SCRIPT_OUTPUT_DIR"] = args.scratchbird_script_output_dir
    if args.scratchbird_monitor_jsonl:
        os.environ["BENCHMARK_SCRATCHBIRD_MONITOR_JSONL"] = args.scratchbird_monitor_jsonl

    # Set default ports
    if args.port is None:
        ports = {'firebird': 3050, 'mysql': 3306, 'postgresql': 5432, 'scratchbird': 3092}
        args.port = ports[args.engine]

    # Create runner
    runner = StressTestRunner(
        engine=args.engine,
        host=args.host,
        port=args.port,
        database=args.database,
        user=args.user,
        password=args.password,
        output_dir=args.output_dir,
        transaction_mode=args.transaction_mode,
    )
    results_file: Optional[Path] = None
    setup_error = ""

    try:
        # Connect
        runner.connect()

        # Generate dataset specification
        dataset = generate_standard_dataset(args.scale)

        # Create schema and load data
        if not args.skip_data_load:
            runner.create_schema(dataset)
            runner.load_data(dataset)
            runner.create_workload_indexes()

            # Verify data integrity
            if not runner.verify_data(dataset):
                raise RuntimeError("Data verification failed")

        # Run stress tests
        runner.run_all_tests(args.test_filter, args.test_set)

        # Print summary
        runner.print_summary()

    except Exception as exc:  # noqa: BLE001 - preserve comparable failure artifact.
        setup_error = str(exc)
        print(f"\nERROR: {setup_error}")
        traceback.print_exc()
        runner.metrics.append(TestMetrics(
            test_name="benchmark_setup",
            description="Benchmark setup/load/test harness failure",
            status="error",
            start_time=time.time(),
            end_time=time.time(),
            duration_ms=0.0,
            error_message=setup_error,
            verification_passed=False,
        ))

    finally:
        results_file = runner.save_results()
        runner.disconnect()
    if results_file:
        print(f"stress_test_runner_result={results_file}")
    has_result_errors = any(m.status in {"failed", "error", "timeout"} for m in runner.metrics)
    has_load_errors = any(m.status != "success" for m in runner.load_metrics)
    if args.fail_on_error and (setup_error or has_result_errors or has_load_errors):
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
