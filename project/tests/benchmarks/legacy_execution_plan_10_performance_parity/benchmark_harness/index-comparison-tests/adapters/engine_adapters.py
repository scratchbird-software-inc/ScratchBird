#!/usr/bin/env python3
"""Engine adapters for the index comparison suite."""

from __future__ import annotations

import json
import os
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from statistics import mean
from typing import Any, Dict, Iterable, List, Optional, Sequence


def ensure_scratchbird_driver() -> None:
    candidates = []
    env_path = Path(__file__).resolve().parents[2] / ".env"
    if env_path.exists():
        for line in env_path.read_text(encoding="utf-8").splitlines():
            if line.startswith("SCRATCHBIRD_DRIVER_PYTHONPATH="):
                candidates.append(Path(line.split("=", 1)[1].strip()))
                break

    for root_candidate in Path(__file__).resolve().parents:
        in_tree_driver = root_candidate / "project" / "drivers" / "driver" / "python" / "src"
        if (in_tree_driver / "scratchbird").exists():
            candidates.append(in_tree_driver)
            break

    harness_root = Path(__file__).resolve().parents[2]
    for root_candidate in harness_root.parents:
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


def percentile(sorted_values: Sequence[float], pct: float) -> float:
    if not sorted_values:
        return 0.0
    index = (len(sorted_values) - 1) * pct
    lower = int(index)
    upper = min(lower + 1, len(sorted_values) - 1)
    if lower == upper:
        return float(sorted_values[lower])
    fraction = index - lower
    return float(sorted_values[lower] + (sorted_values[upper] - sorted_values[lower]) * fraction)


@dataclass
class NormalizedPlan:
    plan_capture_status: str
    raw_plan: Any
    actual_plan_family: str
    used_index: bool
    index_names: List[str]
    fallback_to_scan: bool
    extra_sort: bool
    order_satisfied_by_index: Optional[bool]


class BaseAdapter:
    placeholder = "%s"

    def __init__(self, host: str, port: int, database: str, user: str, password: str):
        self.host = host
        self.port = port
        self.database = database
        self.user = user
        self.password = password
        self.connection = None

    def connect(self) -> None:
        raise NotImplementedError

    def close(self) -> None:
        if self.connection is not None:
            self.connection.close()
            self.connection = None

    def commit(self) -> None:
        self.connection.commit()

    def rollback(self) -> None:
        self.connection.rollback()

    def cursor(self):
        return self.connection.cursor()

    def execute(self, sql: str, fetch: bool = False):
        cursor = self.cursor()
        cursor.execute(sql)
        rows = cursor.fetchall() if fetch else None
        cursor.close()
        return rows

    def execute_many(self, sql: str, rows: Iterable[Sequence[Any]]) -> None:
        cursor = self.cursor()
        cursor.executemany(sql, list(rows))
        cursor.close()

    def drop_table(self, table_name: str) -> None:
        raise NotImplementedError

    def explain(self, query_sql: str) -> NormalizedPlan:
        raise NotImplementedError

    def refresh_planner_statistics(self, table_name: str) -> None:
        return None

    def query_fetch_all(self, query_sql: str) -> List[Sequence[Any]]:
        cursor = self.cursor()
        cursor.execute(query_sql)
        rows = cursor.fetchall()
        cursor.close()
        return rows

    def insert_statement(self, table_name: str, column_count: int) -> str:
        placeholders = ", ".join([self.placeholder] * column_count)
        return f"INSERT INTO {table_name} VALUES ({placeholders})"


class PostgreSQLAdapter(BaseAdapter):
    def connect(self) -> None:
        import psycopg2

        self.connection = psycopg2.connect(
            host=self.host,
            port=self.port,
            dbname=self.database,
            user=self.user,
            password=self.password
        )
        self.connection.autocommit = False

    def drop_table(self, table_name: str) -> None:
        self.execute(f"DROP TABLE IF EXISTS {table_name}")
        self.commit()

    def refresh_planner_statistics(self, table_name: str) -> None:
        self.execute(f"ANALYZE {table_name}")
        self.commit()

    def _collect_nodes(self, node: Dict[str, Any], collected: List[Dict[str, Any]]) -> None:
        collected.append(node)
        for child_key in ("Plans", "Inner Plan", "Outer Plan"):
            child = node.get(child_key)
            if isinstance(child, list):
                for entry in child:
                    if isinstance(entry, dict):
                        self._collect_nodes(entry, collected)
            elif isinstance(child, dict):
                self._collect_nodes(child, collected)

    def explain(self, query_sql: str) -> NormalizedPlan:
        cursor = self.cursor()
        cursor.execute(f"EXPLAIN (FORMAT JSON) {query_sql}")
        raw = cursor.fetchone()[0]
        cursor.close()
        if isinstance(raw, str):
            raw = json.loads(raw)

        root = raw[0]["Plan"]
        nodes: List[Dict[str, Any]] = []
        self._collect_nodes(root, nodes)
        node_types = [str(node.get("Node Type", "")) for node in nodes]
        index_names = [str(node.get("Index Name")) for node in nodes if node.get("Index Name")]
        used_index = any("Index" in node_type or "Bitmap" in node_type for node_type in node_types)
        fallback = any(node_type == "Seq Scan" for node_type in node_types) and not used_index
        extra_sort = any(node_type == "Sort" for node_type in node_types)

        if any(node_type in ("Bitmap Heap Scan", "Bitmap Index Scan") for node_type in node_types):
            plan_family = "bitmap_scan"
        elif any(node_type == "Index Only Scan" for node_type in node_types):
            plan_family = "index_only_scan"
        elif any(node_type == "Index Scan" for node_type in node_types):
            plan_family = "index_scan"
        elif any(node_type == "Seq Scan" for node_type in node_types):
            plan_family = "full_scan"
        else:
            plan_family = "unknown"

        return NormalizedPlan(
            plan_capture_status="ok",
            raw_plan=raw,
            actual_plan_family=plan_family,
            used_index=used_index,
            index_names=index_names,
            fallback_to_scan=fallback,
            extra_sort=extra_sort,
            order_satisfied_by_index=not extra_sort
        )


class MySQLAdapter(BaseAdapter):
    def connect(self) -> None:
        import pymysql

        self.connection = pymysql.connect(
            host=self.host,
            port=self.port,
            database=self.database,
            user=self.user,
            password=self.password,
            autocommit=False,
            charset="utf8mb4"
        )

    def drop_table(self, table_name: str) -> None:
        self.execute(f"DROP TABLE IF EXISTS {table_name}")
        self.commit()

    def refresh_planner_statistics(self, table_name: str) -> None:
        self.execute(f"ANALYZE TABLE {table_name}")
        self.commit()

    def _collect_table_nodes(self, value: Any, collected: List[Dict[str, Any]]) -> None:
        if isinstance(value, dict):
            if "table_name" in value or "access_type" in value:
                collected.append(value)
            for child in value.values():
                self._collect_table_nodes(child, collected)
        elif isinstance(value, list):
            for child in value:
                self._collect_table_nodes(child, collected)

    def _find_filesort(self, value: Any) -> bool:
        if isinstance(value, dict):
            for key, child in value.items():
                if key == "using_filesort" and bool(child):
                    return True
                if self._find_filesort(child):
                    return True
        elif isinstance(value, list):
            for child in value:
                if self._find_filesort(child):
                    return True
        return False

    def explain(self, query_sql: str) -> NormalizedPlan:
        cursor = self.cursor()
        cursor.execute(f"EXPLAIN FORMAT=JSON {query_sql}")
        raw_text = cursor.fetchone()[0]
        cursor.close()

        raw = json.loads(raw_text)
        tables: List[Dict[str, Any]] = []
        self._collect_table_nodes(raw, tables)
        primary = tables[0] if tables else {}
        access_type = str(primary.get("access_type", "UNKNOWN")).lower()
        key_name = primary.get("key")
        used_index = bool(key_name) and access_type != "all"
        fallback = access_type == "all"
        extra_sort = self._find_filesort(raw)

        if access_type in ("const", "eq_ref", "ref", "index_subquery"):
            plan_family = "index_lookup"
        elif access_type == "range":
            plan_family = "range_scan"
        elif access_type == "index":
            plan_family = "index_full_scan"
        elif access_type == "all":
            plan_family = "full_scan"
        else:
            plan_family = "unknown"

        return NormalizedPlan(
            plan_capture_status="ok",
            raw_plan=raw,
            actual_plan_family=plan_family,
            used_index=used_index,
            index_names=[str(key_name)] if key_name else [],
            fallback_to_scan=fallback,
            extra_sort=extra_sort,
            order_satisfied_by_index=not extra_sort
        )


class FirebirdAdapter(BaseAdapter):
    placeholder = "?"

    def connect(self) -> None:
        import fdb

        self.connection = fdb.connect(
            host=self.host,
            port=self.port,
            database=self.database,
            user=self.user,
            password=self.password
        )

    def drop_table(self, table_name: str) -> None:
        try:
            self.execute(f"DROP TABLE {table_name}")
            self.commit()
        except Exception:
            self.rollback()

    def refresh_planner_statistics(self, table_name: str) -> None:
        relation_name = table_name.replace("'", "''").upper()
        cursor = self.cursor()
        cursor.execute(
            f"""
            SELECT TRIM(RDB$INDEX_NAME)
            FROM RDB$INDICES
            WHERE RDB$RELATION_NAME = '{relation_name}'
              AND COALESCE(RDB$SYSTEM_FLAG, 0) = 0
            ORDER BY RDB$INDEX_NAME
            """
        )
        index_names = [row[0].strip() for row in cursor.fetchall() if row and row[0]]
        cursor.close()
        for index_name in index_names:
            quoted_index_name = '"' + index_name.replace('"', '""') + '"'
            self.execute(f"SET STATISTICS INDEX {quoted_index_name}")
        self.commit()

    def explain(self, query_sql: str) -> NormalizedPlan:
        cursor = self.cursor()
        plan = ""
        try:
            prepared = cursor.prep(query_sql)
            plan = prepared.plan or ""
        except Exception:
            plan = ""
        finally:
            cursor.close()

        plan_upper = plan.upper()
        index_names: List[str] = []
        for match in re.findall(r"INDEX\s*\(([^)]+)\)", plan_upper):
            for item in match.split(","):
                cleaned = item.strip()
                if cleaned:
                    index_names.append(cleaned)

        used_index = "INDEX" in plan_upper
        fallback = "NATURAL" in plan_upper and not used_index
        extra_sort = "SORT" in plan_upper

        if used_index:
            plan_family = "index_scan"
        elif fallback:
            plan_family = "full_scan"
        else:
            plan_family = "unknown"

        return NormalizedPlan(
            plan_capture_status="ok" if plan else "error",
            raw_plan=plan,
            actual_plan_family=plan_family,
            used_index=used_index,
            index_names=index_names,
            fallback_to_scan=fallback,
            extra_sort=extra_sort,
            order_satisfied_by_index=(not extra_sort) if plan else None
        )


class ScratchBirdAdapter(BaseAdapter):
    placeholder = "?"

    def connect(self) -> None:
        ensure_scratchbird_driver()
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
        self.connection.autocommit = False
        leak_detector = getattr(self.connection, "_leak_detector", None)
        if leak_detector is not None and hasattr(leak_detector, "config"):
            leak_detector.config.threshold = max(float(leak_detector.config.threshold), 1800.0)

    def execute_many(self, sql: str, rows: Iterable[Sequence[Any]]) -> None:
        row_list = list(rows)
        if not row_list:
            return

        cursor = self.cursor()
        try:
            cursor.executemany(sql, row_list)
        except Exception:
            self.connection.rollback()
            raise
        finally:
            cursor.close()

    def refresh_planner_statistics(self, table_name: str) -> None:
        self.execute(f"ANALYZE {table_name}")
        self.commit()

    def drop_table(self, table_name: str) -> None:
        try:
            self.execute(f"DROP TABLE IF EXISTS {table_name}")
            self.commit()
        except Exception:
            self.rollback()

    def _collect_nodes(self, node: Dict[str, Any], collected: List[Dict[str, Any]]) -> None:
        collected.append(node)
        for child in node.get("children", []):
            if isinstance(child, dict):
                self._collect_nodes(child, collected)

    def explain(self, query_sql: str) -> NormalizedPlan:
        cursor = self.cursor()
        cursor.execute(f"EXPLAIN (FORMAT JSON) {query_sql}")
        raw = cursor.fetchone()[0]
        cursor.close()

        if isinstance(raw, str):
            raw = json.loads(raw)
        if not isinstance(raw, dict):
            return NormalizedPlan(
                plan_capture_status="error",
                raw_plan=raw,
                actual_plan_family="unknown",
                used_index=False,
                index_names=[],
                fallback_to_scan=False,
                extra_sort=False,
                order_satisfied_by_index=None,
            )

        root = raw.get("plan_root", {})
        nodes: List[Dict[str, Any]] = []
        if isinstance(root, dict):
            self._collect_nodes(root, nodes)

        node_types = [str(node.get("node_type", "")) for node in nodes]
        index_names = [str(node.get("index_name")) for node in nodes if node.get("index_name")]
        used_index = any(node_type in ("IndexScan", "IndexOnlyScan", "BitmapIndexScan") for node_type in node_types)
        fallback = any(node_type == "SeqScan" for node_type in node_types) and not used_index
        extra_sort = any(node_type == "Sort" for node_type in node_types)

        if "BitmapIndexScan" in node_types:
            plan_family = "bitmap_scan"
        elif "IndexOnlyScan" in node_types:
            plan_family = "index_only_scan"
        elif "IndexScan" in node_types:
            plan_family = "index_scan"
        elif "SeqScan" in node_types:
            plan_family = "full_scan"
        else:
            plan_family = "unknown"

        return NormalizedPlan(
            plan_capture_status="ok",
            raw_plan=raw,
            actual_plan_family=plan_family,
            used_index=used_index,
            index_names=index_names,
            fallback_to_scan=fallback,
            extra_sort=extra_sort,
            order_satisfied_by_index=not extra_sort,
        )


def create_adapter(engine: str, host: str, port: int, database: str, user: str, password: str) -> BaseAdapter:
    adapters = {
        "firebird": FirebirdAdapter,
        "mysql": MySQLAdapter,
        "postgresql": PostgreSQLAdapter,
        "scratchbird": ScratchBirdAdapter,
    }
    adapter = adapters[engine](host, port, database, user, password)
    adapter.connect()
    return adapter


def summarize_latencies(latencies_ms: Sequence[float]) -> Dict[str, float]:
    ordered = sorted(float(value) for value in latencies_ms)
    return {
        "latency_avg_ms": round(mean(ordered), 3) if ordered else 0.0,
        "latency_p95_ms": round(percentile(ordered, 0.95), 3),
        "latency_p99_ms": round(percentile(ordered, 0.99), 3),
        "throughput_qps": round((1000.0 * len(ordered) / sum(ordered)), 3) if ordered and sum(ordered) > 0 else 0.0
    }
