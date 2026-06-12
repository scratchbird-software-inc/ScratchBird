#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate and emit public performance/capacity baseline evidence."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import sys
from typing import Any


# PUBLIC_CAPACITY_BASELINE_REPORT
# PUBLIC_PERFORMANCE_BASELINE_GATE

REQUIRED_OPERATIONS = {
    "create_open_database",
    "dml_insert_batch",
    "dml_select_batch",
    "index_lookup",
    "optimizer_planning_plan_cache",
    "transaction_begin_commit",
    "listener_accept_to_handoff",
    "parser_worker_pool_exhaustion",
    "backup_create",
    "restore_verify",
    "sweep_mga_cleanup",
    "archive_slice",
    "memory_pressure",
    "support_bundle_generation",
    "evidence_overhead_budget",
}

REQUIRED_ENTERPRISE_METRICS = (
    "mean_ms",
    "median_ms",
    "p50_ms",
    "p90_ms",
    "p95_ms",
    "p99_ms",
    "p99_9_ms",
    "max_latency_ms",
    "error_rate_max",
    "retry_rate_max",
    "spill_rate_max",
    "cleanup_lag_max",
    "memory_high_water_bytes_max",
    "fd_high_water_max",
)

FORBIDDEN_REFERENCE_FRAGMENTS = (
    "docs" + "/" + "execution-plans",
    "docs" + "/" + "completed-execution-plans",
    "docs" + "/" + "findings",
    "." + "git",
    "/" + "home" + "/" + "dcalford",
    "ScratchBird" + "-Private",
    "local" + "_work",
)

FORBIDDEN_CLAIM_FRAGMENTS = (
    "faster than",
    "outperform",
    "beats ",
    "dominates",
    "compatibility dominance",
    "reference engine comparison",
    "firebird",
    "postgresql",
    "mysql",
    "private cluster production",
)


def fail(message: str) -> None:
    print(f"public_performance_baseline_gate=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def reject_private_reference(value: str, context: str) -> None:
    if Path(value).is_absolute():
        fail(f"absolute_path_recorded:{context}:{value}")
    for fragment in FORBIDDEN_REFERENCE_FRAGMENTS:
        if fragment in value:
            fail(f"private_reference_recorded:{context}:{value}")


def reject_claim_text(value: str, context: str) -> None:
    lowered = value.lower()
    for fragment in FORBIDDEN_CLAIM_FRAGMENTS:
        if fragment in lowered:
            fail(f"forbidden_claim_text:{context}:{fragment}")


def reject_strings_recursive(value: Any, context: str) -> None:
    if isinstance(value, str):
        reject_private_reference(value, context)
        reject_claim_text(value, context)
    elif isinstance(value, list):
        for index, item in enumerate(value):
            reject_strings_recursive(item, f"{context}[{index}]")
    elif isinstance(value, dict):
        for key, item in value.items():
            reject_private_reference(str(key), f"{context}.key")
            reject_strings_recursive(item, f"{context}.{key}")


def load_json(path: Path) -> tuple[dict[str, Any], str]:
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        fail(f"read_failed:{path}:{exc}")
    try:
        loaded = json.loads(text)
    except json.JSONDecodeError as exc:
        fail(f"json_invalid:{path}:{exc}")
    require(isinstance(loaded, dict), f"json_root_not_object:{path}")
    return loaded, text


def number(value: Any, context: str) -> float:
    require(isinstance(value, (int, float)) and not isinstance(value, bool), f"number_required:{context}")
    return float(value)


def validate_threshold_source(repo_root: Path, suite: dict[str, Any]) -> dict[str, Any]:
    policy = suite.get("measurement_policy")
    require(isinstance(policy, dict), "measurement_policy_missing")
    source = policy.get("source_thresholds")
    require(isinstance(source, str) and source, "source_thresholds_missing")
    reject_private_reference(source, "source_thresholds")
    threshold_path = repo_root / source
    thresholds, text = load_json(threshold_path)
    reject_strings_recursive(thresholds, "thresholds")
    require(
        thresholds.get("schema") == "scratchbird.beta.performance.baseline.v1",
        "threshold_schema_invalid",
    )
    threshold_values = thresholds.get("thresholds")
    require(isinstance(threshold_values, dict), "threshold_values_missing")
    for key in (
        "startup_open_latency_ms_max",
        "session_begin_commit_latency_ms_max",
        "simple_query_latency_ms_max",
        "insert_rows_per_second_min",
        "select_rows_per_second_min",
        "rss_growth_bytes_max",
        "repeat_count",
        "sample_rows",
    ):
        require(key in threshold_values, f"threshold_key_missing:{key}")
    return {
        "source": source,
        "sha256": sha256_text(text),
        "threshold_count": len(threshold_values),
    }


def validate_policy(suite: dict[str, Any]) -> None:
    require(suite.get("schema_version") == 2, "schema_version_invalid")
    require(suite.get("marker") == "PUBLIC_PERFORMANCE_BASELINES", "marker_missing")
    require(suite.get("authority") == "public_release_evidence_only", "authority_invalid")
    policy = suite.get("measurement_policy")
    require(isinstance(policy, dict), "measurement_policy_missing")
    require(policy.get("mode") == "enterprise_release_candidate_public_thresholds", "policy_mode_invalid")
    require(policy.get("comparative_reference_engine_claims") is False, "comparative_claims_enabled")
    require(policy.get("parser_sql_text_authority") is False, "parser_authority_drift")
    require(policy.get("cluster_production_claims") is False, "cluster_claims_enabled")
    require(policy.get("private_inputs_required") is False, "private_inputs_required")
    require(policy.get("runtime_measurement_required_for_release_gate") is True, "runtime_policy_drift")
    required_fields = suite.get("required_metric_fields")
    require(isinstance(required_fields, list), "required_metric_fields_missing")
    missing_fields = sorted(set(REQUIRED_ENTERPRISE_METRICS) - set(required_fields))
    require(not missing_fields, "required_metric_fields_missing:" + ",".join(missing_fields))


def validate_baseline(repo_root: Path, baseline: dict[str, Any]) -> dict[str, Any]:
    operation_id = baseline.get("operation_id")
    surface = baseline.get("surface")
    require(isinstance(operation_id, str) and operation_id, "operation_id_invalid")
    require(isinstance(surface, str) and surface, f"surface_invalid:{operation_id}")
    require(operation_id in REQUIRED_OPERATIONS, f"operation_unexpected:{operation_id}")
    for key in REQUIRED_ENTERPRISE_METRICS:
        value = number(baseline.get(key), f"{operation_id}.{key}")
        if key.endswith("_max"):
            require(value >= 0, f"metric_negative:{operation_id}:{key}")
        else:
            require(value > 0, f"metric_nonpositive:{operation_id}:{key}")
    p50 = number(baseline.get("p50_ms"), f"{operation_id}.p50_ms")
    p90 = number(baseline.get("p90_ms"), f"{operation_id}.p90_ms")
    p95 = number(baseline.get("p95_ms"), f"{operation_id}.p95_ms")
    p99 = number(baseline.get("p99_ms"), f"{operation_id}.p99_ms")
    p99_9 = number(baseline.get("p99_9_ms"), f"{operation_id}.p99_9_ms")
    max_latency = number(baseline.get("max_latency_ms"), f"{operation_id}.max_latency_ms")
    require(p50 <= p90 <= p95 <= p99 <= p99_9 <= max_latency,
            f"percentile_order_invalid:{operation_id}")
    capacity = baseline.get("capacity_limits")
    require(isinstance(capacity, dict) and capacity, f"capacity_limits_missing:{operation_id}")
    positive_capacity = 0
    for key, value in capacity.items():
        require(isinstance(key, str) and key, f"capacity_key_invalid:{operation_id}")
        limit = number(value, f"{operation_id}.{key}")
        require(limit >= 0, f"capacity_limit_negative:{operation_id}:{key}")
        if limit > 0:
            positive_capacity += 1
    require(positive_capacity > 0, f"capacity_positive_limit_missing:{operation_id}")
    require(baseline.get("measurement_quality") == "enterprise_release_candidate_threshold", f"quality_invalid:{operation_id}")
    require(baseline.get("benchmark_clean") is True, f"benchmark_clean_false:{operation_id}")
    boundary = baseline.get("authority_boundary")
    require(isinstance(boundary, str) and boundary.endswith("_evidence_only") or boundary == "mga_transaction_inventory_authority_only",
            f"authority_boundary_invalid:{operation_id}")
    anchor = baseline.get("public_test_anchor")
    require(isinstance(anchor, str) and anchor, f"public_test_anchor_missing:{operation_id}")
    reject_private_reference(anchor, f"public_test_anchor:{operation_id}")
    require((repo_root / anchor).is_file(), f"public_test_anchor_missing_on_disk:{operation_id}:{anchor}")
    return {
        "operation_id": operation_id,
        "surface": surface,
        "p50_ms": p50,
        "p95_ms": p95,
        "p99_ms": p99,
        "capacity_keys": ";".join(sorted(capacity)),
        "capacity_limit_count": len(capacity),
        "measurement_quality": baseline["measurement_quality"],
        "benchmark_clean": "true",
        "authority_boundary": boundary,
        "public_test_anchor": anchor,
    }


def validate_baselines(repo_root: Path, suite: dict[str, Any]) -> list[dict[str, Any]]:
    baselines = suite.get("baselines")
    require(isinstance(baselines, list) and baselines, "baselines_missing")
    records = []
    seen = set()
    for item in baselines:
        require(isinstance(item, dict), "baseline_not_object")
        record = validate_baseline(repo_root, item)
        operation_id = record["operation_id"]
        require(operation_id not in seen, f"operation_duplicate:{operation_id}")
        seen.add(operation_id)
        records.append(record)
    missing = sorted(REQUIRED_OPERATIONS - seen)
    extra = sorted(seen - REQUIRED_OPERATIONS)
    require(not missing, "required_operations_missing:" + ",".join(missing))
    require(not extra, "unexpected_operations:" + ",".join(extra))
    return records


def write_csv(path: Path, records: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=[
                "operation_id",
                "surface",
                "p50_ms",
                "p95_ms",
                "p99_ms",
                "capacity_limit_count",
                "capacity_keys",
                "measurement_quality",
                "benchmark_clean",
                "authority_boundary",
                "public_test_anchor",
            ],
        )
        writer.writeheader()
        writer.writerows(records)


def write_evidence(path: Path, evidence: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--csv-output", required=True, type=Path)
    parser.add_argument("--evidence-output", required=True, type=Path)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    suite, text = load_json(args.baseline)
    reject_strings_recursive(suite, "baseline")
    validate_policy(suite)
    threshold_record = validate_threshold_source(repo_root, suite)
    records = validate_baselines(repo_root, suite)
    write_csv(args.csv_output, records)
    csv_text = args.csv_output.read_text(encoding="utf-8")
    try:
        baseline_path = args.baseline.resolve().relative_to(repo_root).as_posix()
    except ValueError:
        fail(f"baseline_path_not_under_repo:{args.baseline}")
    evidence = {
        "schema": "scratchbird.public.performance_baseline_gate.v1",
        "marker": "PUBLIC_CAPACITY_BASELINE_REPORT",
        "baseline_path": baseline_path,
        "baseline_sha256": sha256_text(text),
        "csv_sha256": sha256_text(csv_text),
        "threshold_source": threshold_record,
        "operation_count": len(records),
        "required_operations": sorted(REQUIRED_OPERATIONS),
        "comparative_reference_engine_claims": False,
        "authority": "public_release_evidence_only",
    }
    write_evidence(args.evidence_output, evidence)
    print(
        "public_performance_baseline_gate=passed "
        f"operations={len(records)} "
        f"baseline_sha256={evidence['baseline_sha256']} "
        f"csv_sha256={evidence['csv_sha256']}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
