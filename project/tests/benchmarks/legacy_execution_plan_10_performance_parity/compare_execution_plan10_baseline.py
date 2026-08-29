#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate and record the current ScratchBird Execution_Plan 10 baseline.

This lane has one current-engine result and no independently reproduced
cross-engine baseline.  It therefore records a strict semantic baseline and
the observed measurements, but deliberately makes no performance-parity or
superiority claim.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import sys
from pathlib import Path
from typing import Any


SCHEMA_VERSION = "scratchbird.execution_plan10.current_baseline.v1"
CURRENT_SOURCE_SET = "current_scratchbird"
EXPECTED_TRANSACTION_MODE = "normal_transactional"
EXPECTED_ADAPTER = "scratchbird_current_native_v1"
EXPECTED_SCHEMA_PATH = "users.public.current_native_benchmark"
EXPECTED_LOAD_ROWS = {
    "customers": 10_000,
    "products": 5_000,
    "orders": 50_000,
    "order_items": 200_000,
}
EXPECTED_TESTS = {
    "inner_join_simple",
    "inner_join_large_result",
    "inner_join_multiple_conditions",
    "left_join_all_customers",
    "four_table_join",
    "self_join_same_country",
    "bulk_update_with_join",
}


class BaselineError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise BaselineError(message)


def integer(value: Any, field: str) -> int:
    require(not isinstance(value, bool), f"{field} must be an integer")
    try:
        converted = int(value)
    except (TypeError, ValueError) as exc:
        raise BaselineError(f"{field} must be an integer") from exc
    require(converted == value, f"{field} must be an exact integer")
    return converted


def finite_nonnegative(value: Any, field: str) -> float:
    try:
        converted = float(value)
    except (TypeError, ValueError) as exc:
        raise BaselineError(f"{field} must be numeric") from exc
    require(math.isfinite(converted) and converted >= 0.0,
            f"{field} must be finite and nonnegative")
    return converted


def canonical_json(value: Any) -> bytes:
    return json.dumps(
        value,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def load_candidates(root: Path) -> list[tuple[Path, dict[str, Any]]]:
    candidates: list[tuple[Path, dict[str, Any]]] = []
    for path in sorted(root.rglob("stress_scratchbird_normal_transactional_*.json")):
        if path.name.endswith(".lane.json"):
            continue
        try:
            payload = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            raise BaselineError(f"invalid stress result JSON {path}: {exc}") from exc
        require(isinstance(payload, dict), f"stress result is not an object: {path}")
        candidates.append((path, payload))
    require(len(candidates) == 1,
            f"expected exactly one current ScratchBird stress result, found {len(candidates)}")
    return candidates


def validate_payload(payload: dict[str, Any]) -> dict[str, Any]:
    metadata = payload.get("metadata")
    summary = payload.get("summary")
    require(isinstance(metadata, dict), "metadata object is missing")
    require(isinstance(summary, dict), "summary object is missing")
    lane = metadata.get("execution_lane_provenance")
    require(isinstance(lane, dict), "execution_lane_provenance object is missing")

    require(metadata.get("engine") == "scratchbird",
            "result engine is not scratchbird")
    require(metadata.get("dialect") == "scratchbird",
            "result dialect is not scratchbird")
    require(metadata.get("transaction_mode") == EXPECTED_TRANSACTION_MODE,
            "result transaction mode drifted")
    require(metadata.get("requested_transaction_mode") == EXPECTED_TRANSACTION_MODE,
            "requested transaction mode drifted")
    require(metadata.get("benchmark_surface_adapter") == EXPECTED_ADAPTER,
            "benchmark surface adapter drifted")
    require(metadata.get("schema_path_profile") == EXPECTED_SCHEMA_PATH,
            "schema path profile drifted")
    require(lane.get("schema_version") == "scratchbird_benchmarks.execution_lane.v3",
            "execution lane schema drifted")
    require(lane.get("suite") == "stress-tests",
            "result is not the stress-tests lane")
    require(lane.get("lane_class") ==
            "scratchbird_current_native_workplan10_equivalent_lane",
            "execution lane class drifted")
    require(lane.get("benchmark_surface_adapter") == EXPECTED_ADAPTER,
            "execution lane adapter drifted")
    require(lane.get("schema_path_profile") == EXPECTED_SCHEMA_PATH,
            "execution lane schema path drifted")
    require("sb_isql" in str(lane.get("load_mechanism", "")) and
            "sb_isql" in str(lane.get("prepared_or_batch_behavior", "")),
            "result does not preserve sb_isql execution provenance")
    require("ingest_throughput" in lane.get("waived_claims", []),
            "portable lane did not retain the ingest-throughput waiver")

    loading = payload.get("data_loading")
    require(isinstance(loading, list), "data_loading array is missing")
    load_by_name = {
        row.get("table_name"): row
        for row in loading
        if isinstance(row, dict) and isinstance(row.get("table_name"), str)
    }
    require(set(load_by_name) == set(EXPECTED_LOAD_ROWS),
            "data-loading table set drifted")
    normalized_loads: list[dict[str, Any]] = []
    for table_name, expected_rows in EXPECTED_LOAD_ROWS.items():
        row = load_by_name[table_name]
        require(row.get("status") == "success",
                f"data load failed for {table_name}")
        require(integer(row.get("row_count"), f"{table_name}.row_count") == expected_rows,
                f"data load row count drifted for {table_name}")
        duration_ms = finite_nonnegative(
            row.get("duration_ms"), f"{table_name}.duration_ms")
        rows_per_second = finite_nonnegative(
            row.get("rows_per_second"), f"{table_name}.rows_per_second")
        normalized_loads.append({
            "table_name": table_name,
            "row_count": expected_rows,
            "status": "success",
            "duration_ms": duration_ms,
            "rows_per_second": rows_per_second,
        })

    tests = payload.get("test_results")
    require(isinstance(tests, list), "test_results array is missing")
    test_by_name = {
        row.get("test_name"): row
        for row in tests
        if isinstance(row, dict) and isinstance(row.get("test_name"), str)
    }
    require(set(test_by_name) == EXPECTED_TESTS,
            "Execution_Plan 10 test set drifted")
    normalized_tests: list[dict[str, Any]] = []
    for test_name in sorted(EXPECTED_TESTS):
        row = test_by_name[test_name]
        require(row.get("status") == "passed", f"{test_name} did not pass")
        require(row.get("verification_passed") is True,
                f"{test_name} lacks independent verification evidence")
        duration_ms = finite_nonnegative(
            row.get("duration_ms"), f"{test_name}.duration_ms")
        rows_returned = integer(
            row.get("rows_returned"), f"{test_name}.rows_returned")
        require(rows_returned >= 0, f"{test_name}.rows_returned is negative")
        normalized_tests.append({
            "test_name": test_name,
            "status": "passed",
            "verification_passed": True,
            "rows_returned": rows_returned,
            "duration_ms": duration_ms,
        })

    require(integer(summary.get("total_tests"), "summary.total_tests") ==
            len(EXPECTED_TESTS), "summary total_tests drifted")
    require(integer(summary.get("passed"), "summary.passed") ==
            len(EXPECTED_TESTS), "summary passed count drifted")
    require(integer(summary.get("failed"), "summary.failed") == 0,
            "summary contains failed tests")
    require(integer(summary.get("errors"), "summary.errors") == 0,
            "summary contains errors")
    total_duration_ms = finite_nonnegative(
        summary.get("total_duration_ms"), "summary.total_duration_ms")

    timestamp = metadata.get("timestamp")
    require(isinstance(timestamp, str) and timestamp,
            "result timestamp is missing")
    return {
        "timestamp": timestamp,
        "loads": normalized_loads,
        "tests": normalized_tests,
        "total_duration_ms": total_duration_ms,
    }


def write_csv(path: Path, normalized: dict[str, Any]) -> None:
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(
            stream,
            fieldnames=(
                "record_kind",
                "record_name",
                "row_count",
                "status",
                "verification_passed",
                "duration_ms",
                "rows_per_second",
            ),
        )
        writer.writeheader()
        for row in normalized["loads"]:
            writer.writerow({
                "record_kind": "data_load",
                "record_name": row["table_name"],
                "row_count": row["row_count"],
                "status": row["status"],
                "verification_passed": "true",
                "duration_ms": row["duration_ms"],
                "rows_per_second": row["rows_per_second"],
            })
        for row in normalized["tests"]:
            writer.writerow({
                "record_kind": "stress_test",
                "record_name": row["test_name"],
                "row_count": row["rows_returned"],
                "status": row["status"],
                "verification_passed": "true",
                "duration_ms": row["duration_ms"],
                "rows_per_second": "",
            })


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--current-result-root", required=True, type=Path)
    parser.add_argument("--current-source-set", required=True)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args(argv[1:])

    try:
        require(args.current_source_set == CURRENT_SOURCE_SET,
                f"unsupported current source set: {args.current_source_set!r}")
        result_root = args.current_result_root.resolve()
        require(result_root.is_dir(),
                f"current result root is unavailable: {result_root}")
        result_path, payload = load_candidates(result_root)[0]
        normalized = validate_payload(payload)

        semantic_projection = {
            "schema_version": SCHEMA_VERSION,
            "current_source_set": CURRENT_SOURCE_SET,
            "transaction_mode": EXPECTED_TRANSACTION_MODE,
            "adapter": EXPECTED_ADAPTER,
            "schema_path": EXPECTED_SCHEMA_PATH,
            "loads": [
                {key: row[key] for key in ("table_name", "row_count", "status")}
                for row in normalized["loads"]
            ],
            "tests": [
                {
                    key: row[key]
                    for key in (
                        "test_name",
                        "status",
                        "verification_passed",
                        "rows_returned",
                    )
                }
                for row in normalized["tests"]
            ],
        }
        artifact = {
            "schema_version": SCHEMA_VERSION,
            "current_source_set": CURRENT_SOURCE_SET,
            "comparison_status": "current_baseline_recorded_no_comparative_claim",
            "comparison_performed": False,
            "performance_parity_claim": False,
            "performance_superiority_claim": False,
            "claim_ceiling": (
                "current ScratchBird semantic and measurement baseline only; "
                "no cross-engine or historical performance comparison"
            ),
            "result_path": str(result_path),
            "result_sha256": sha256_bytes(result_path.read_bytes()),
            "semantic_projection_sha256": sha256_bytes(
                canonical_json(semantic_projection)),
            "semantic_projection": semantic_projection,
            "observed_measurements": normalized,
        }
        output_dir = args.output_dir.resolve()
        output_dir.mkdir(parents=True, exist_ok=True)
        json_path = output_dir / "execution_plan10-current-baseline.json"
        csv_path = output_dir / "execution_plan10-current-baseline.csv"
        json_path.write_text(
            json.dumps(artifact, ensure_ascii=False, sort_keys=True, indent=2) + "\n",
            encoding="utf-8",
        )
        write_csv(csv_path, normalized)
        print(json.dumps({
            "comparison_status": artifact["comparison_status"],
            "performance_parity_claim": False,
            "result": str(result_path),
            "json_artifact": str(json_path),
            "csv_artifact": str(csv_path),
            "semantic_projection_sha256": artifact["semantic_projection_sha256"],
        }, indent=2))
        return 0
    except BaselineError as exc:
        print(f"compare_execution_plan10_baseline=failed: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
