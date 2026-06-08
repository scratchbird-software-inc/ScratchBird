#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate that a ScratchBird benchmark result is Execution_Plan 10 comparable."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any


REQUIRED_LOAD_ROWS = {
    "customers": 10000,
    "products": 5000,
    "orders": 50000,
    "order_items": 200000,
}

SCRATCHBIRD_CURRENT_SURFACE_ADAPTER = "scratchbird_current_native_v1"
SCRATCHBIRD_CURRENT_SCHEMA_PATH_PROFILE = "users.public.current_native_benchmark"
SCRATCHBIRD_CURRENT_TABLES = {
    "customers": "users.public.benchmark_customers",
    "products": "users.public.benchmark_products",
    "orders": "users.public.benchmark_orders",
    "order_items": "users.public.benchmark_order_items",
}

REQUIRED_TESTS = {
    "inner_join_simple",
    "inner_join_large_result",
    "inner_join_multiple_conditions",
    "left_join_all_customers",
    "four_table_join",
    "self_join_same_country",
    "bulk_update_with_join",
}

MICRO_TESTS = {"single_insert", "point_select", "simple_aggregate"}


def load_json(path: Path) -> dict[str, Any] | None:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    return payload if isinstance(payload, dict) else None


def iter_json_payloads(root: Path):
    for path in sorted(root.rglob("*.json")):
        if path.name.endswith(".lane.json") or path.name in {
            "system-info.json",
            "comparison-guard.json",
            "run-provenance.json",
            "matrix-run-provenance.json",
        }:
            continue
        payload = load_json(path)
        if payload is not None:
            yield path, payload


def looks_like_micro_result(payload: dict[str, Any]) -> bool:
    if payload.get("suite") == "micro":
        return True
    result_names = {
        str(item.get("test_name"))
        for item in payload.get("results", [])
        if isinstance(item, dict)
    }
    return bool(result_names & MICRO_TESTS)


def looks_like_execution_plan10_result(payload: dict[str, Any]) -> bool:
    return all(key in payload for key in ("metadata", "summary", "data_loading", "test_results"))


def int_value(value: Any) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return 0


def validate_execution_plan10_payload(path: Path, payload: dict[str, Any]) -> list[str]:
    blockers: list[str] = []
    metadata = payload.get("metadata") if isinstance(payload.get("metadata"), dict) else {}
    summary = payload.get("summary") if isinstance(payload.get("summary"), dict) else {}
    lane = metadata.get("execution_lane_provenance") if isinstance(metadata.get("execution_lane_provenance"), dict) else {}

    engine = str(metadata.get("engine") or metadata.get("engine_name") or "")
    if engine != "scratchbird":
        blockers.append(f"{path}: engine is {engine!r}, expected 'scratchbird'")

    transaction_mode = str(metadata.get("transaction_mode") or metadata.get("requested_transaction_mode") or "")
    if transaction_mode != "normal_transactional":
        blockers.append(f"{path}: transaction_mode is {transaction_mode!r}, expected 'normal_transactional'")

    if lane.get("suite") != "stress-tests":
        blockers.append(f"{path}: execution lane is not the Execution_Plan 10 stress-tests lane")
    lane_text = " ".join(str(lane.get(key, "")) for key in ("load_mechanism", "prepared_or_batch_behavior", "driver_name"))
    if engine == "scratchbird" and "sb_isql" not in lane_text:
        blockers.append(f"{path}: ScratchBird current run does not declare sb_isql script execution evidence")
    if engine == "scratchbird":
        adapter = str(metadata.get("benchmark_surface_adapter") or lane.get("benchmark_surface_adapter") or "")
        if adapter != SCRATCHBIRD_CURRENT_SURFACE_ADAPTER:
            blockers.append(
                f"{path}: ScratchBird current run uses adapter {adapter!r}, "
                f"expected {SCRATCHBIRD_CURRENT_SURFACE_ADAPTER!r}"
            )
        schema_path_profile = str(metadata.get("schema_path_profile") or lane.get("schema_path_profile") or "")
        if schema_path_profile != SCRATCHBIRD_CURRENT_SCHEMA_PATH_PROFILE:
            blockers.append(
                f"{path}: ScratchBird schema_path_profile is {schema_path_profile!r}, "
                f"expected {SCRATCHBIRD_CURRENT_SCHEMA_PATH_PROFILE!r}"
            )

    load_by_table = {
        str(item.get("table_name")): item
        for item in payload.get("data_loading", [])
        if isinstance(item, dict)
    }
    for table_name, expected_rows in REQUIRED_LOAD_ROWS.items():
        item = load_by_table.get(table_name)
        if item is None:
            blockers.append(f"{path}: missing data load row for {table_name}")
            continue
        if item.get("status") != "success":
            blockers.append(f"{path}: data load for {table_name} did not succeed: {item.get('error_message', '')}")
        actual_rows = int_value(item.get("row_count"))
        if actual_rows != expected_rows:
            blockers.append(f"{path}: data load for {table_name} has {actual_rows} rows, expected {expected_rows}")
        if engine == "scratchbird":
            physical_table = str(item.get("physical_table_name") or "")
            expected_physical_table = SCRATCHBIRD_CURRENT_TABLES[table_name]
            if physical_table != expected_physical_table:
                blockers.append(
                    f"{path}: data load for {table_name} used {physical_table!r}, "
                    f"expected current path {expected_physical_table!r}"
                )

    tests_by_name = {
        str(item.get("test_name")): item
        for item in payload.get("test_results", [])
        if isinstance(item, dict)
    }
    for test_name in sorted(REQUIRED_TESTS):
        item = tests_by_name.get(test_name)
        if item is None:
            blockers.append(f"{path}: missing required Execution_Plan 10 test {test_name}")
            continue
        if item.get("status") != "passed":
            blockers.append(f"{path}: {test_name} status is {item.get('status')!r}: {item.get('error_message', '')}")
        if not bool(item.get("verification_passed")):
            blockers.append(f"{path}: {test_name} did not record verification_passed=true")

    setup_error = tests_by_name.get("benchmark_setup")
    if setup_error is not None and setup_error.get("status") != "passed":
        blockers.append(f"{path}: benchmark setup failed: {setup_error.get('error_message', '')}")

    if int_value(summary.get("failed")) or int_value(summary.get("errors")):
        blockers.append(
            f"{path}: summary has failed={summary.get('failed')} errors={summary.get('errors')}"
        )

    return blockers


def scan_result_root(root: Path) -> dict[str, Any]:
    micro_results: list[str] = []
    candidates: list[dict[str, Any]] = []
    blockers: list[str] = []

    for path, payload in iter_json_payloads(root):
        if looks_like_micro_result(payload):
            micro_results.append(str(path))
        if not looks_like_execution_plan10_result(payload):
            continue
        payload_blockers = validate_execution_plan10_payload(path, payload)
        candidate = {
            "path": str(path),
            "timestamp": str((payload.get("metadata") or {}).get("timestamp") or ""),
            "blockers": payload_blockers,
        }
        candidates.append(candidate)
        blockers.extend(payload_blockers)

    comparable_candidates = [candidate for candidate in candidates if not candidate["blockers"]]
    selected = max(comparable_candidates, key=lambda item: (item["timestamp"], item["path"]), default=None)

    if not candidates:
        blockers.append(f"{root}: no Execution_Plan 10 stress-result JSON was found")
    if micro_results and not selected:
        blockers.append(
            f"{root}: micro benchmark results are route-health evidence only and are not Execution_Plan 10 comparable"
        )

    return {
        "result_root": str(root),
        "comparable": selected is not None,
        "selected_result": selected["path"] if selected else None,
        "candidate_count": len(candidates),
        "micro_result_count": len(micro_results),
        "blockers": blockers,
        "candidates": candidates,
    }


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--result-root", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    expectation = parser.add_mutually_exclusive_group()
    expectation.add_argument("--expect-comparable", action="store_true")
    expectation.add_argument("--expect-incomparable", action="store_true")
    args = parser.parse_args(argv[1:])

    result = scan_result_root(args.result_root.resolve())
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2), encoding="utf-8")

    print(json.dumps(result, indent=2))

    if args.expect_comparable and not result["comparable"]:
        return 1
    if args.expect_incomparable and result["comparable"]:
        return 1
    if args.expect_incomparable and not result["blockers"]:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
