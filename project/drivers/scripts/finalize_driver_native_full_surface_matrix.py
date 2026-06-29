#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Finalize sharded driver-native full-surface matrix evidence."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


def load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def result_rows(report: dict[str, Any]) -> list[dict[str, Any]]:
    full_results_path = report.get("full_results_path")
    if isinstance(full_results_path, str) and full_results_path:
        path = Path(full_results_path)
        if path.is_file():
            rows = load_json(path)
            if isinstance(rows, list):
                return [row for row in rows if isinstance(row, dict)]
    rows = report.get("results")
    if isinstance(rows, list):
        return [row for row in rows if isinstance(row, dict)]
    return []


def failure_rows(report: dict[str, Any]) -> list[dict[str, Any]]:
    full_failures_path = report.get("full_failures_path")
    if isinstance(full_failures_path, str) and full_failures_path:
        path = Path(full_failures_path)
        if path.is_file():
            rows = load_json(path)
            if isinstance(rows, list):
                return [row for row in rows if isinstance(row, dict)]
    return [row for row in result_rows(report) if row.get("status") not in {"pass", "planned"}]


def combination_id(row: dict[str, Any]) -> str:
    value = row.get("combination_id")
    if isinstance(value, str) and value:
        return value
    ordinal = row.get("combination_ordinal", "")
    return "|".join(
        str(part)
        for part in (
            ordinal,
            row.get("driver", ""),
            row.get("route", ""),
            row.get("sslmode", ""),
            row.get("page_size", ""),
            row.get("parser_mode", ""),
            row.get("concurrency_mode", ""),
        )
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--expected-plan", type=Path)
    parser.add_argument("--shard-report", type=Path, action="append")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--generate-deterministic-fixture", action="store_true")
    parser.add_argument("--fixture-root", type=Path)
    args = parser.parse_args()

    if args.generate_deterministic_fixture:
        fixture_root = args.fixture_root or args.output.parent / "matrix-finalizer-fixture"
        fixture_root.mkdir(parents=True, exist_ok=True)
        expected_plan = fixture_root / "expected-plan.json"
        shard_results = fixture_root / "shard-results.json"
        shard_report = fixture_root / "shard-report.json"
        row = {
            "combination_id": "1|cpp|ipc_local|disable|16k|server-parser|single",
            "combination_ordinal": 1,
            "driver": "cpp",
            "route": "ipc_local",
            "sslmode": "disable",
            "page_size": "16k",
            "parser_mode": "server-parser",
            "concurrency_mode": "single",
            "status": "planned",
        }
        expected_plan.write_text(
            json.dumps({"results": [row], "full_results_path": str(fixture_root / "expected-results.json")}, indent=2)
            + "\n",
            encoding="utf-8",
        )
        (fixture_root / "expected-results.json").write_text(json.dumps([row], indent=2) + "\n", encoding="utf-8")
        passed = dict(row)
        passed["status"] = "pass"
        shard_results.write_text(json.dumps([passed], indent=2) + "\n", encoding="utf-8")
        shard_report.write_text(
            json.dumps(
                {
                    "status": "pass",
                    "full_results_path": str(shard_results),
                    "full_failures_path": str(fixture_root / "shard-failures.json"),
                    "combination_count": 1,
                    "selected_combination_count": 1,
                    "executed_count": 1,
                    "failure_count": 0,
                    "shard_count": 1,
                    "shard_index": 0,
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
        (fixture_root / "shard-failures.json").write_text("[]\n", encoding="utf-8")
        args.expected_plan = expected_plan
        args.shard_report = [shard_report]

    if args.expected_plan is None:
        raise SystemExit("--expected-plan is required unless --generate-deterministic-fixture is used")
    if not args.shard_report:
        raise SystemExit("--shard-report is required unless --generate-deterministic-fixture is used")
    expected_report = load_json(args.expected_plan)
    expected_rows = result_rows(expected_report)
    expected = {
        combination_id(row): row
        for row in expected_rows
        if row.get("status") == "planned"
    }
    observed_passes: dict[str, dict[str, Any]] = {}
    observed_failures: dict[str, dict[str, Any]] = {}
    duplicate_passes: list[str] = []
    shard_summaries: list[dict[str, Any]] = []
    for shard_path in args.shard_report:
        report = load_json(shard_path)
        rows = result_rows(report)
        failures = failure_rows(report)
        shard_summaries.append(
            {
                "path": str(shard_path),
                "status": report.get("status"),
                "combination_count": report.get("combination_count"),
                "selected_combination_count": report.get("selected_combination_count"),
                "executed_count": report.get("executed_count"),
                "planned_count": report.get("planned_count"),
                "failure_count": report.get("failure_count"),
                "shard_count": report.get("shard_count"),
                "shard_index": report.get("shard_index"),
            }
        )
        for row in rows:
            cid = combination_id(row)
            if row.get("status") == "pass":
                if cid in observed_passes:
                    duplicate_passes.append(cid)
                observed_passes[cid] = row
        for row in failures:
            observed_failures[combination_id(row)] = row

    missing = sorted(set(expected) - set(observed_passes))
    unexpected = sorted(set(observed_passes) - set(expected))
    failed_expected = sorted(set(expected) & set(observed_failures))
    report = {
        "schema_version": "scratchbird_driver_native_full_surface_matrix_final_v1",
        "status": "pass" if not (missing or unexpected or failed_expected or duplicate_passes) else "fail",
        "expected_plan": str(args.expected_plan),
        "expected_count": len(expected),
        "observed_pass_count": len(observed_passes),
        "observed_failure_count": len(observed_failures),
        "missing_count": len(missing),
        "unexpected_count": len(unexpected),
        "failed_expected_count": len(failed_expected),
        "duplicate_pass_count": len(duplicate_passes),
        "missing": missing[:500],
        "unexpected": unexpected[:500],
        "failed_expected": failed_expected[:500],
        "duplicate_passes": duplicate_passes[:500],
        "shards": shard_summaries,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(
        f"driver_native_full_surface_matrix_final={report['status']} "
        f"expected={len(expected)} observed_pass={len(observed_passes)}"
    )
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
