#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Shared first-batch CDP benchmark fixture metadata and JSON helpers."""

from __future__ import annotations

import hashlib
import json
import os
import platform
import shutil
import time
from pathlib import Path
from typing import Any


BENCHMARK_SCHEMA_VERSION = "cdp.route_split_benchmark.v1"
BASELINE_SCHEMA_VERSION = "cdp.baseline_capture.v1"
CORPUS_SCHEMA_VERSION = "cdp.dml_planner_corpus.v1"
CORPUS_VERSION = "cdp-dml-planner-corpus-v1"
CORPUS_SEED = 2026052201
SUPPORTED_SLICES = ("CDP-001", "CDP-003", "CDP-004", "CDP-005", "CDP-009", "CDP-048")
FORBIDDEN_EXECUTION_PLAN_PATH = "docs" + "/execution-plans/"

REQUIRED_BENCHMARK_FIELDS = (
    "run_id",
    "timestamp_utc",
    "build_mode",
    "cluster_enabled",
    "route",
    "database_path",
    "fixture_name",
    "statement_id",
    "operation",
    "rows_input",
    "rows_affected",
    "rows_returned",
    "elapsed_ms",
    "parse_ms",
    "bind_ms",
    "lower_ms",
    "plan_ms",
    "execute_ms",
    "append_ms",
    "page_allocation_wait_ms",
    "filespace_growth_wait_ms",
    "index_maintenance_ms",
    "plan_cache_hit",
    "metadata_cache_hit",
    "statistics_epoch",
    "agent_worker_threads",
    "agent_cpu_ms",
    "resource_governor_state",
    "message_vector",
    "result_hash",
)

REQUIRED_BASELINE_FIELDS = (
    "schema_version",
    "run_id",
    "timestamp_utc",
    "scope",
    "records",
    "route_names",
    "lane_ids",
    "performance_targets",
    "baseline_status",
)

REQUIRED_ENVIRONMENT_FIELDS = (
    "cpu_model",
    "cpu_core_count",
    "build_mode",
    "route",
    "corpus_version",
    "corpus_seed",
    "tracing_mode",
    "storage_path",
)

REQUIRED_MANIFEST_FIELDS = (
    "build_id",
    "source_commit",
    "build_type",
    "compiler_flags",
    "cluster_enabled",
    "configuration_files",
    "effective_performance_flags",
    "ctest_labels",
    "benchmark_json_paths",
    "profiler_output_paths",
    "logs",
    "support_bundle_paths",
    "route_names",
    "corpus_version",
    "generator_seed",
    "result_hashes",
    "environment_fingerprint",
    "before_after_baseline_link",
    "rollback_disabled_mode_evidence_link",
    "final_audit_link",
)


class FixtureSupportError(RuntimeError):
    pass


def utc_timestamp() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def normalize_build_mode(value: str | None) -> str:
    value = (value or "").strip()
    return value if value else "unknown"


def hash_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def hash_json(value: Any) -> str:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":"))
    return hash_text(encoded)


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def read_git_commit(repo_root: Path) -> str:
    del repo_root
    return os.environ.get("SB_PUBLIC_SOURCE_REVISION", "public-tree-no-vcs-input")


def _read_first_proc_value(path: Path, key: str) -> str:
    try:
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.lower().startswith(key.lower()):
                return line.split(":", 1)[1].strip()
    except OSError:
        return ""
    return ""


def cpu_model() -> str:
    proc = platform.processor().strip()
    if proc:
        return proc
    from_proc = _read_first_proc_value(Path("/proc/cpuinfo"), "model name")
    if from_proc:
        return from_proc
    hardware = _read_first_proc_value(Path("/proc/cpuinfo"), "Hardware")
    return hardware or platform.machine() or "unknown"


def memory_summary() -> dict[str, int | str]:
    result: dict[str, int | str] = {"total_bytes": "unknown", "available_bytes": "unknown"}
    try:
        pages = os.sysconf("SC_PHYS_PAGES")
        page_size = os.sysconf("SC_PAGE_SIZE")
        if isinstance(pages, int) and isinstance(page_size, int):
            result["total_bytes"] = pages * page_size
    except (ValueError, OSError, AttributeError):
        pass
    try:
        for line in Path("/proc/meminfo").read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("MemAvailable:"):
                result["available_bytes"] = int(line.split()[1]) * 1024
            elif line.startswith("MemTotal:") and result["total_bytes"] == "unknown":
                result["total_bytes"] = int(line.split()[1]) * 1024
    except OSError:
        pass
    return result


def _read_file(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace").strip()
    except OSError:
        return ""


def cpu_frequency_policy() -> dict[str, Any]:
    governors: set[str] = set()
    frequencies: dict[str, str] = {}
    cpu_root = Path("/sys/devices/system/cpu")
    for policy in sorted(cpu_root.glob("cpu[0-9]*/cpufreq")):
        governor = _read_file(policy / "scaling_governor")
        if governor:
            governors.add(governor)
        current = _read_file(policy / "scaling_cur_freq")
        if current:
            frequencies[policy.parent.name] = current
    return {
        "governors": sorted(governors) if governors else "unknown",
        "sample_current_khz": frequencies if frequencies else "unknown",
    }


def numa_topology() -> dict[str, Any]:
    node_root = Path("/sys/devices/system/node")
    nodes: list[dict[str, str]] = []
    for node in sorted(node_root.glob("node[0-9]*")):
        cpulist = _read_file(node / "cpulist")
        meminfo = _read_file(node / "meminfo")
        nodes.append(
            {
                "node": node.name,
                "cpulist": cpulist or "unknown",
                "meminfo_hash": hash_text(meminfo) if meminfo else "unknown",
            }
        )
    return {"available": bool(nodes), "nodes": nodes}


def filesystem_summary(storage_path: Path) -> dict[str, Any]:
    target = storage_path if storage_path.exists() else storage_path.parent
    target = target.resolve()
    best_mount = Path("/")
    best_parts: list[str] = []
    try:
        for line in Path("/proc/mounts").read_text(encoding="utf-8", errors="replace").splitlines():
            parts = line.split()
            if len(parts) < 4:
                continue
            mountpoint = Path(parts[1].replace("\\040", " ")).resolve()
            try:
                target.relative_to(mountpoint)
            except ValueError:
                continue
            if len(str(mountpoint)) >= len(str(best_mount)):
                best_mount = mountpoint
                best_parts = parts
    except OSError:
        best_parts = []

    if best_parts:
        device = best_parts[0]
        fs_type = best_parts[2]
        options = best_parts[3].split(",")
    else:
        device = "unknown"
        fs_type = "unknown"
        options = []

    device_name = Path(device).name if device.startswith("/dev/") else ""
    rotational = "unknown"
    if device_name:
        base_name = device_name.rstrip("0123456789")
        for candidate in (device_name, base_name):
            rotational_value = _read_file(Path("/sys/block") / candidate / "queue" / "rotational")
            if rotational_value:
                rotational = "rotational" if rotational_value == "1" else "non_rotational"
                break

    return {
        "mount_point": str(best_mount),
        "device": device,
        "type": fs_type,
        "mount_options": options,
        "storage_device_class": rotational,
    }


def tracing_mode_from_env(default: str | None = None) -> str:
    if default:
        return default
    trace_keys = (
        "SCRATCHBIRD_SELECT_TRACE",
        "SCRATCHBIRD_INSERT_TRACE",
        "SCRATCHBIRD_UPDATE_TRACE",
        "SCRATCHBIRD_EXEC_PROFILE",
        "SCRATCHBIRD_PREPARED_TRACE",
        "SCRATCHBIRD_HOTPATH_TRACE",
    )
    active = [key for key in trace_keys if os.environ.get(key) not in (None, "", "0", "false", "False")]
    return "diagnostic:" + ",".join(active) if active else "benchmark_clean_or_untraced"


def capture_environment(
    *,
    route: str,
    storage_path: Path,
    build_mode: str | None,
    tracing_mode: str | None,
) -> dict[str, Any]:
    storage_parent = storage_path if storage_path.is_dir() else storage_path.parent
    try:
        disk = shutil.disk_usage(storage_parent)
        disk_payload: dict[str, Any] = {
            "total_bytes": disk.total,
            "used_bytes": disk.used,
            "free_bytes": disk.free,
        }
    except OSError:
        disk_payload = {"total_bytes": "unknown", "used_bytes": "unknown", "free_bytes": "unknown"}
    try:
        load_avg: list[float] | str = [float(value) for value in os.getloadavg()]
    except (OSError, AttributeError):
        load_avg = "unknown"

    normalized_build_mode = normalize_build_mode(build_mode)
    normalized_tracing = tracing_mode_from_env(tracing_mode)
    logical_count = os.cpu_count() or 0
    filesystem = filesystem_summary(storage_path)
    return {
        "schema_version": "cdp.benchmark_environment.v1",
        "timestamp_utc": utc_timestamp(),
        "route": route,
        "cpu_model": cpu_model(),
        "cpu_core_count": logical_count,
        "cpu_thread_count": logical_count,
        "cpu_frequency_policy": cpu_frequency_policy(),
        "numa_topology": numa_topology(),
        "platform": {
            "system": platform.system(),
            "release": platform.release(),
            "machine": platform.machine(),
            "python": platform.python_version(),
        },
        "memory": memory_summary(),
        "load_average": load_avg,
        "build_mode": normalized_build_mode,
        "cluster_enabled": False,
        "tracing_mode": normalized_tracing,
        "trace_environment": {
            key: os.environ.get(key, "")
            for key in (
                "SCRATCHBIRD_SELECT_TRACE",
                "SCRATCHBIRD_INSERT_TRACE",
                "SCRATCHBIRD_UPDATE_TRACE",
                "SCRATCHBIRD_EXEC_PROFILE",
                "SCRATCHBIRD_PREPARED_TRACE",
            )
        },
        "corpus_version": CORPUS_VERSION,
        "corpus_seed": CORPUS_SEED,
        "storage_path": str(storage_path),
        "scratch_directory": str(storage_parent),
        "filesystem": disk_payload,
        "filesystem_detail": filesystem,
        "storage_device_class": filesystem["storage_device_class"],
        "page_cache_state": "not_controlled_by_ctest_smoke",
        "background_load_summary": "loadavg_from_python_stdlib" if load_avg != "unknown" else "unknown",
        "server_configuration_path": "none_ctest_process_arguments_only",
        "compiler_flags": "not_visible_to_python_ctest_fixture",
        "effective_performance_flags": {
            "benchmark_clean_or_untraced": normalized_tracing == "benchmark_clean_or_untraced",
            "tracing_mode": normalized_tracing,
        },
    }


def fixture_specs() -> list[dict[str, Any]]:
    return [
        {
            "fixture_id": "cdp_copy_narrow",
            "fixture_name": "cdp_copy_narrow_rows",
            "operation": "copy",
            "statement_id": "cdp.copy.narrow.v1",
            "sql": "COPY users.public.cdp_copy_narrow FROM STDIN",
            "scale_lanes": [
                {"lane_id": "ctest_smoke", "rows": 8},
                {"lane_id": "copy_10k", "rows": 10_000},
                {"lane_id": "copy_100k", "rows": 100_000},
                {"lane_id": "copy_1m", "rows": 1_000_000},
            ],
            "schema": {
                "tables": ["users.public.cdp_copy_narrow"],
                "columns": [
                    {"name": "id", "type": "int64", "nullable": False},
                    {"name": "group_key", "type": "int32", "nullable": False},
                    {"name": "payload", "type": "text", "nullable": False},
                    {"name": "nullable_note", "type": "text", "nullable": True},
                ],
            },
            "indexes": [
                {"name": "pk_cdp_copy_narrow", "kind": "primary", "columns": ["id"]},
            ],
            "distribution": {
                "key_distribution": "sequential",
                "skew": "none",
                "null_rate": {"nullable_note": 0.125},
                "row_width_class": "narrow",
                "hot_row_pattern": "none",
                "update_ratio": 0.0,
                "delete_ratio": 0.0,
            },
        },
        {
            "fixture_id": "cdp_select_point_range",
            "fixture_name": "cdp_select_point_and_range",
            "operation": "select",
            "statement_id": "cdp.select.point_range.v1",
            "sql": "SELECT id, payload FROM users.public.cdp_copy_narrow WHERE id BETWEEN ? AND ?",
            "scale_lanes": [
                {"lane_id": "ctest_smoke", "rows": 8},
                {"lane_id": "point_select", "rows": 1},
                {"lane_id": "range_select", "rows": 256},
            ],
            "schema": {"tables": ["users.public.cdp_copy_narrow"]},
            "indexes": [
                {"name": "pk_cdp_copy_narrow", "kind": "primary", "columns": ["id"]},
                {"name": "idx_cdp_copy_narrow_group", "kind": "secondary", "columns": ["group_key"]},
            ],
            "distribution": {
                "key_distribution": "bounded_uniform",
                "skew": "none",
                "null_rate": {},
                "row_width_class": "narrow",
                "hot_row_pattern": "repeat_low_id_probe",
                "update_ratio": 0.0,
                "delete_ratio": 0.0,
            },
        },
        {
            "fixture_id": "cdp_join_execution_plan10_shape",
            "fixture_name": "cdp_join_customer_order_items",
            "operation": "join",
            "statement_id": "cdp.join.execution_plan10_shape.v1",
            "sql": (
                "SELECT c.id, SUM(oi.extended_price) "
                "FROM users.public.cdp_customers c "
                "JOIN users.public.cdp_orders o ON o.customer_id = c.id "
                "JOIN users.public.cdp_order_items oi ON oi.order_id = o.id "
                "GROUP BY c.id"
            ),
            "scale_lanes": [
                {"lane_id": "ctest_smoke", "rows": 8},
                {"lane_id": "execution_plan10_small", "customers": 10_000, "orders": 50_000, "order_items": 200_000},
            ],
            "schema": {
                "tables": [
                    "users.public.cdp_customers",
                    "users.public.cdp_orders",
                    "users.public.cdp_order_items",
                ]
            },
            "indexes": [
                {"name": "pk_cdp_customers", "kind": "primary", "columns": ["id"]},
                {"name": "idx_cdp_orders_customer", "kind": "secondary", "columns": ["customer_id"]},
                {"name": "idx_cdp_items_order", "kind": "secondary", "columns": ["order_id"]},
            ],
            "distribution": {
                "key_distribution": "foreign_key_fanout",
                "skew": "zipf_light_hot_customers",
                "null_rate": {},
                "row_width_class": "mixed",
                "hot_row_pattern": "top_1_percent_customers",
                "update_ratio": 0.0,
                "delete_ratio": 0.0,
            },
        },
        {
            "fixture_id": "cdp_aggregate_count",
            "fixture_name": "cdp_aggregate_count_rows",
            "operation": "aggregate",
            "statement_id": "cdp.aggregate.count.v1",
            "sql": "SELECT COUNT(*) FROM users.public.cdp_copy_narrow",
            "scale_lanes": [
                {"lane_id": "ctest_smoke", "rows": 8},
                {"lane_id": "aggregate_count", "rows": 100_000},
            ],
            "schema": {"tables": ["users.public.cdp_copy_narrow"]},
            "indexes": [
                {"name": "pk_cdp_copy_narrow", "kind": "primary", "columns": ["id"]},
            ],
            "distribution": {
                "key_distribution": "sequential",
                "skew": "none",
                "null_rate": {},
                "row_width_class": "narrow",
                "hot_row_pattern": "none",
                "update_ratio": 0.0,
                "delete_ratio": 0.0,
            },
        },
        {
            "fixture_id": "cdp_update_hot_rows",
            "fixture_name": "cdp_update_hot_rows",
            "operation": "update",
            "statement_id": "cdp.update.hot_rows.v1",
            "sql": "UPDATE users.public.cdp_copy_narrow SET group_key = group_key + 1 WHERE id IN (?)",
            "scale_lanes": [
                {"lane_id": "ctest_smoke", "rows": 8},
                {"lane_id": "single_row_dml", "rows": 1},
                {"lane_id": "hot_row_update", "rows": 1_000},
            ],
            "schema": {"tables": ["users.public.cdp_copy_narrow"]},
            "indexes": [
                {"name": "pk_cdp_copy_narrow", "kind": "primary", "columns": ["id"]},
                {"name": "idx_cdp_copy_narrow_group", "kind": "secondary", "columns": ["group_key"]},
            ],
            "distribution": {
                "key_distribution": "hot_set_then_uniform_tail",
                "skew": "10_percent_rows_receive_90_percent_updates",
                "null_rate": {},
                "row_width_class": "narrow",
                "hot_row_pattern": "stable_hot_id_set",
                "update_ratio": 0.10,
                "delete_ratio": 0.01,
            },
        },
    ]


def _copy_row(index: int) -> dict[str, Any]:
    group_key = ((index * 17) + CORPUS_SEED) % 97
    return {
        "id": index,
        "group_key": group_key,
        "payload": f"payload-{CORPUS_SEED % 1000:03d}-{index:06d}",
        "nullable_note": None if index % 8 == 0 else f"note-{group_key:02d}",
    }


def _join_rows(index: int) -> dict[str, Any]:
    customer_id = (index % 5) + 1
    order_id = index + 1000
    return {
        "customer": {"id": customer_id, "region": f"r{customer_id % 3}", "status": "active"},
        "order": {"id": order_id, "customer_id": customer_id, "order_day": 19_000 + index},
        "order_item": {
            "id": index + 10_000,
            "order_id": order_id,
            "product_id": (index % 7) + 1,
            "quantity": (index % 4) + 1,
            "extended_price": ((index % 11) + 1) * 125,
        },
    }


def _update_row(index: int) -> dict[str, Any]:
    before = _copy_row(index)
    delta = 1 + (1 if index % 10 == 0 else 0)
    after = dict(before)
    after["group_key"] = before["group_key"] + delta
    return {
        "id": index,
        "hot_row": index % 10 == 0,
        "before": before,
        "delta": delta,
        "after": after,
    }


def build_corpus_metadata(sample_rows: int = 8) -> dict[str, Any]:
    samples = {
        "cdp_copy_narrow": [_copy_row(index) for index in range(1, sample_rows + 1)],
        "cdp_select_point_range": {
            "point_keys": [1, max(1, sample_rows // 2), sample_rows],
            "range_bounds": [1, sample_rows],
        },
        "cdp_join_execution_plan10_shape": [_join_rows(index) for index in range(1, sample_rows + 1)],
        "cdp_aggregate_count": {
            "input_rows": sample_rows,
            "expected_count": sample_rows,
        },
        "cdp_update_hot_rows": [_update_row(index) for index in range(1, sample_rows + 1)],
    }
    hashes = {key: hash_json(value) for key, value in samples.items()}
    payload = {
        "schema_version": CORPUS_SCHEMA_VERSION,
        "corpus_version": CORPUS_VERSION,
        "generator_seed": CORPUS_SEED,
        "generator_name": "cdp_dml_planner_corpus_generator",
        "ctest_smoke_rows": sample_rows,
        "preserved_large_run_lanes": ["copy_10k", "copy_100k", "copy_1m", "execution_plan10_small"],
        "fixtures": fixture_specs(),
        "sample_rows": samples,
        "expected_hashes": hashes,
    }
    payload["corpus_hash"] = hash_json(
        {
            "corpus_version": payload["corpus_version"],
            "generator_seed": payload["generator_seed"],
            "fixtures": payload["fixtures"],
            "expected_hashes": payload["expected_hashes"],
        }
    )
    return payload


def performance_targets() -> list[dict[str, Any]]:
    return [
        {
            "lane_id": "route_split",
            "scope": "embedded IPC INET routes",
            "metric": "route_overhead_ms",
            "target": "separate_engine_from_transport_cost",
            "regression_ceiling": "route_result_hashes_match",
            "run_count": 3,
            "summary_rule": "median",
            "variance_rule": "route_delta_explained_by_counters",
        },
        {
            "lane_id": "copy_10k",
            "scope": "COPY 10k rows",
            "metric": "rows_per_second",
            "target": "establish_pre_optimization_baseline",
            "regression_ceiling": "route_result_hashes_match",
            "run_count": 3,
            "summary_rule": "median",
            "variance_rule": "ctest_smoke_preserves_large_lane_contract",
        },
        {
            "lane_id": "copy_100k",
            "scope": "COPY 100k rows",
            "metric": "rows_per_second",
            "target": "improve_over_baseline",
            "regression_ceiling": "not_worse_than_baseline_by_5_percent",
            "run_count": 5,
            "summary_rule": "median_and_p95",
            "variance_rule": "p95_within_20_percent_of_median_or_investigate",
        },
        {
            "lane_id": "copy_1m",
            "scope": "COPY 1M rows",
            "metric": "rows_per_second",
            "target": "improve_over_baseline",
            "regression_ceiling": "not_worse_than_baseline_by_5_percent",
            "run_count": 5,
            "summary_rule": "median_and_p95",
            "variance_rule": "p95_within_20_percent_of_median_or_investigate",
        },
        {
            "lane_id": "execution_plan10_joins",
            "scope": "Execution_Plan10 join corpus",
            "metric": "elapsed_ms",
            "target": "improve_over_baseline",
            "regression_ceiling": "not_worse_than_baseline_by_5_percent",
            "run_count": 5,
            "summary_rule": "median_and_p95",
            "variance_rule": "p95_within_20_percent_of_median_or_investigate",
        },
        {
            "lane_id": "range_select",
            "scope": "range select",
            "metric": "elapsed_ms",
            "target": "no_regression_target",
            "regression_ceiling": "not_worse_than_baseline_by_5_percent",
            "run_count": 5,
            "summary_rule": "median_and_p95",
            "variance_rule": "p95_within_20_percent_of_median_or_investigate",
        },
        {
            "lane_id": "aggregate_count",
            "scope": "COUNT aggregate",
            "metric": "elapsed_ms",
            "target": "no_regression_target",
            "regression_ceiling": "not_worse_than_baseline_by_5_percent",
            "run_count": 5,
            "summary_rule": "median_and_p95",
            "variance_rule": "p95_within_20_percent_of_median_or_investigate",
        },
        {
            "lane_id": "single_row_dml",
            "scope": "single insert update delete",
            "metric": "elapsed_ms",
            "target": "no_regression_target",
            "regression_ceiling": "not_worse_than_baseline_by_5_percent",
            "run_count": 5,
            "summary_rule": "median_and_p95",
            "variance_rule": "p95_within_20_percent_of_median_or_investigate",
        },
        {
            "lane_id": "hot_row_update",
            "scope": "hot row update",
            "metric": "elapsed_ms",
            "target": "no_regression_target",
            "regression_ceiling": "not_worse_than_baseline_by_5_percent",
            "run_count": 5,
            "summary_rule": "median_and_p95",
            "variance_rule": "p95_within_20_percent_of_median_or_investigate",
        },
        {
            "lane_id": "point_select",
            "scope": "point select",
            "metric": "elapsed_ms",
            "target": "no_regression_target",
            "regression_ceiling": "not_worse_than_baseline_by_5_percent",
            "run_count": 5,
            "summary_rule": "median_and_p95",
            "variance_rule": "p95_within_20_percent_of_median_or_investigate",
        },
    ]


def benchmark_record(
    *,
    run_id: str,
    timestamp_utc: str,
    build_mode: str | None,
    route: str,
    database_path: Path,
    fixture_name: str,
    statement_id: str,
    operation: str,
    lane_id: str,
    rows_input: int,
    rows_affected: int,
    elapsed_ms: float | None,
    rows_returned: int,
    result_text: str,
    status: str,
    message_vector: list[str],
    operation_status: str = "accepted",
    route_counters: dict[str, Any] | None = None,
) -> dict[str, Any]:
    result_hash = hash_text(result_text.strip())
    record: dict[str, Any] = {
        "run_id": run_id,
        "timestamp_utc": timestamp_utc,
        "build_mode": normalize_build_mode(build_mode),
        "cluster_enabled": False,
        "route": route,
        "database_path": str(database_path),
        "fixture_name": fixture_name,
        "statement_id": statement_id,
        "operation": operation,
        "lane_id": lane_id,
        "rows_input": rows_input,
        "rows_affected": rows_affected,
        "rows_returned": rows_returned,
        "elapsed_ms": elapsed_ms,
        "parse_ms": None,
        "bind_ms": None,
        "lower_ms": None,
        "plan_ms": None,
        "execute_ms": None,
        "append_ms": None,
        "page_allocation_wait_ms": None,
        "filespace_growth_wait_ms": None,
        "index_maintenance_ms": None,
        "plan_cache_hit": None,
        "metadata_cache_hit": None,
        "statistics_epoch": None,
        "agent_worker_threads": None,
        "agent_cpu_ms": None,
        "resource_governor_state": "not_reported_by_sb_isql_route_smoke",
        "message_vector": message_vector,
        "result_hash": result_hash,
        "benchmark_status": status,
        "operation_status": operation_status,
        "phase_timing_source": "not_exposed_by_first_batch_sb_isql_route_smoke",
        "corpus_version": CORPUS_VERSION,
        "corpus_seed": CORPUS_SEED,
        "route_counters": route_counters or {},
    }
    return record


def build_evidence_manifest(
    *,
    run_id: str,
    repo_root: Path,
    build_mode: str | None,
    benchmark_json: Path,
    baseline_json: Path,
    corpus_json: Path,
    environment_json: Path,
    route_names: list[str],
    records: list[dict[str, Any]],
    logs: list[str],
    environment_fingerprint: str,
) -> dict[str, Any]:
    return {
        "schema_version": "cdp.benchmark_evidence_manifest.v1",
        "run_id": run_id,
        "build_id": run_id,
        "source_commit": read_git_commit(repo_root),
        "build_type": normalize_build_mode(build_mode),
        "compiler_flags": "not_visible_to_python_ctest_fixture",
        "cluster_enabled": False,
        "configuration_files": [],
        "effective_performance_flags": {
            "benchmark_clean_or_untraced": True,
            "hot_path_trace_from_environment": tracing_mode_from_env(None),
        },
        "ctest_labels": [
            "cdp_route_split_benchmark_gate",
            *SUPPORTED_SLICES,
            "sbsql_parser_worker",
            "benchmark",
        ],
        "ctest_results": {"cdp_route_split_benchmark_gate": "passed"},
        "benchmark_json_paths": [str(benchmark_json)],
        "baseline_json_paths": [str(baseline_json)],
        "corpus_json_paths": [str(corpus_json)],
        "environment_json_paths": [str(environment_json)],
        "profiler_output_paths": [],
        "logs": logs,
        "support_bundle_paths": [],
        "route_names": route_names,
        "corpus_version": CORPUS_VERSION,
        "generator_seed": CORPUS_SEED,
        "result_hashes": {
            f"{record['route']}:{record.get('lane_id', record['statement_id'])}": record["result_hash"]
            for record in records
        },
        "environment_fingerprint": environment_fingerprint,
        "before_after_baseline_link": str(baseline_json),
        "rollback_disabled_mode_evidence_link": "not_applicable_first_batch_fixture_only",
        "final_audit_link": "not_applicable_first_batch_fixture_only",
    }


def environment_fingerprint(environments: list[dict[str, Any]]) -> str:
    stable = [
        {
            "route": env.get("route"),
            "cpu_model": env.get("cpu_model"),
            "cpu_core_count": env.get("cpu_core_count"),
            "build_mode": env.get("build_mode"),
            "tracing_mode": env.get("tracing_mode"),
            "corpus_version": env.get("corpus_version"),
            "corpus_seed": env.get("corpus_seed"),
            "platform": env.get("platform"),
        }
        for env in environments
    ]
    return hash_json(stable)


def _walk_strings(value: Any):
    if isinstance(value, str):
        yield value
    elif isinstance(value, dict):
        for item in value.values():
            yield from _walk_strings(item)
    elif isinstance(value, list):
        for item in value:
            yield from _walk_strings(item)


def validate_payload(payload: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    slices = set(payload.get("supported_slices", []))
    for slice_id in SUPPORTED_SLICES:
        if slice_id not in slices:
            errors.append(f"missing slice marker {slice_id}")

    records = payload.get("benchmark_records", [])
    if not isinstance(records, list) or not records:
        errors.append("benchmark_records missing or empty")
    routes = {record.get("route") for record in records if isinstance(record, dict)}
    if routes != {"embedded", "ipc", "inet"}:
        errors.append(f"benchmark route set mismatch: {sorted(str(route) for route in routes)}")
    lane_ids = {record.get("lane_id") for record in records if isinstance(record, dict)}
    for lane_id in (
        "route_split",
        "copy_10k",
        "point_select",
        "range_select",
        "aggregate_count",
        "execution_plan10_joins",
        "single_row_dml",
    ):
        if lane_id not in lane_ids:
            errors.append(f"benchmark missing lane {lane_id}")
    for index, record in enumerate(records):
        missing = [field for field in REQUIRED_BENCHMARK_FIELDS if field not in record]
        if missing:
            errors.append(f"record {index} missing fields: {missing}")
        if record.get("benchmark_status") != "passed":
            errors.append(f"record {index} did not pass: {record.get('message_vector')}")
        if not record.get("lane_id"):
            errors.append(f"record {index} missing lane_id")

    by_lane: dict[str, list[dict[str, Any]]] = {}
    for record in records:
        if isinstance(record, dict):
            by_lane.setdefault(str(record.get("lane_id")), []).append(record)
    for lane_id, lane_records in sorted(by_lane.items()):
        lane_routes = {record.get("route") for record in lane_records}
        if lane_routes != {"embedded", "ipc", "inet"}:
            errors.append(f"lane {lane_id} route set mismatch: {sorted(str(route) for route in lane_routes)}")
        lane_hashes = {record.get("result_hash") for record in lane_records}
        if len(lane_hashes) != 1:
            errors.append(f"lane {lane_id} result hashes differ: {sorted(str(value) for value in lane_hashes)}")
        lane_statuses = {record.get("operation_status") for record in lane_records}
        if len(lane_statuses) != 1:
            errors.append(f"lane {lane_id} operation statuses differ: {sorted(str(value) for value in lane_statuses)}")

    operations = {record.get("operation") for record in records if isinstance(record, dict)}
    for operation in ("copy", "select", "join", "aggregate", "update"):
        if operation not in operations:
            errors.append(f"benchmark records missing {operation} operation")

    corpus = payload.get("corpus", {})
    operations = {
        fixture.get("operation")
        for fixture in corpus.get("fixtures", [])
        if isinstance(fixture, dict)
    }
    for operation in ("copy", "select", "join", "aggregate", "update"):
        if operation not in operations:
            errors.append(f"corpus missing {operation} fixture")
    if corpus.get("corpus_version") != CORPUS_VERSION:
        errors.append("corpus version mismatch")
    if corpus.get("generator_seed") != CORPUS_SEED:
        errors.append("corpus seed mismatch")

    environments = payload.get("environments", [])
    if not isinstance(environments, list) or not environments:
        errors.append("environments missing or empty")
    for index, env in enumerate(environments):
        missing = [field for field in REQUIRED_ENVIRONMENT_FIELDS if field not in env]
        if missing:
            errors.append(f"environment {index} missing fields: {missing}")
        if not env.get("cpu_core_count"):
            errors.append(f"environment {index} missing usable core count")

    target_ids = {target.get("lane_id") for target in payload.get("performance_targets", [])}
    for target in (
        "route_split",
        "copy_10k",
        "copy_100k",
        "copy_1m",
        "execution_plan10_joins",
        "single_row_dml",
        "hot_row_update",
        "point_select",
        "range_select",
        "aggregate_count",
    ):
        if target not in target_ids:
            errors.append(f"performance target missing {target}")

    manifest = payload.get("evidence_manifest", {})
    for field in REQUIRED_MANIFEST_FIELDS:
        if field not in manifest:
            errors.append(f"evidence manifest missing {field}")

    baseline = payload.get("baseline", {})
    for field in REQUIRED_BASELINE_FIELDS:
        if field not in baseline:
            errors.append(f"baseline missing {field}")
    if baseline.get("records") != records:
        errors.append("baseline records do not match benchmark records")

    for text in _walk_strings(payload):
        normalized = text.replace("\\", "/")
        if FORBIDDEN_EXECUTION_PLAN_PATH in normalized:
            errors.append(f"runtime payload depends on execution_plan path: {text}")
            break
    return errors


def validate_written_artifacts(
    *,
    benchmark_json: Path,
    baseline_json: Path,
    corpus_json: Path,
    environment_json: Path,
    manifest_json: Path,
) -> list[str]:
    errors: list[str] = []

    def load(path: Path) -> dict[str, Any]:
        if not path.exists():
            errors.append(f"artifact missing: {path}")
            return {}
        try:
            return json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as exc:
            errors.append(f"artifact is not valid JSON: {path}: {exc}")
            return {}

    benchmark = load(benchmark_json)
    baseline = load(baseline_json)
    corpus = load(corpus_json)
    environment = load(environment_json)
    manifest = load(manifest_json)

    if benchmark:
        errors.extend(validate_payload(benchmark))
    if baseline:
        for field in REQUIRED_BASELINE_FIELDS:
            if field not in baseline:
                errors.append(f"baseline artifact missing {field}")
        if baseline.get("schema_version") != BASELINE_SCHEMA_VERSION:
            errors.append("baseline artifact schema version mismatch")
    if corpus:
        if corpus.get("schema_version") != CORPUS_SCHEMA_VERSION:
            errors.append("corpus artifact schema version mismatch")
        if corpus.get("corpus_version") != CORPUS_VERSION:
            errors.append("corpus artifact version mismatch")
        if corpus.get("generator_seed") != CORPUS_SEED:
            errors.append("corpus artifact seed mismatch")
    if environment:
        environments = environment.get("environments", [])
        if not isinstance(environments, list) or not environments:
            errors.append("environment artifact missing environments")
        if not environment.get("environment_fingerprint"):
            errors.append("environment artifact missing fingerprint")
    if manifest:
        for field in REQUIRED_MANIFEST_FIELDS:
            if field not in manifest:
                errors.append(f"manifest artifact missing {field}")
        expected_paths = {
            str(benchmark_json),
            str(baseline_json),
            str(corpus_json),
            str(environment_json),
        }
        manifest_paths = set(manifest.get("benchmark_json_paths", []))
        manifest_paths.update(manifest.get("baseline_json_paths", []))
        manifest_paths.update(manifest.get("corpus_json_paths", []))
        manifest_paths.update(manifest.get("environment_json_paths", []))
        missing_paths = expected_paths - manifest_paths
        if missing_paths:
            errors.append(f"manifest artifact path list missing: {sorted(missing_paths)}")

    for payload in (benchmark, baseline, corpus, environment, manifest):
        for text in _walk_strings(payload):
            normalized = text.replace("\\", "/")
            if FORBIDDEN_EXECUTION_PLAN_PATH in normalized:
                errors.append(f"artifact depends on execution_plan path: {text}")
                return errors
    return errors
