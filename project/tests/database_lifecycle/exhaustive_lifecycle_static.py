#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""DBLC-016 static/report audit for exhaustive lifecycle regression coverage."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path
import re
import sys


EXECUTION_PLAN = "project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure"
VALIDATION_PLAN = f"{EXECUTION_PLAN}/VALIDATION_PLAN.md"
REGRESSION_REPORT = f"{EXECUTION_PLAN}/artifacts/DATABASE_LIFECYCLE_REGRESSION_REPORT.md"
REGRESSION_MATRIX = f"{EXECUTION_PLAN}/artifacts/DATABASE_LIFECYCLE_REGRESSION_MATRIX.csv"
CTEST_REQUIRED_GATES = f"{EXECUTION_PLAN}/artifacts/DATABASE_LIFECYCLE_CTEST_REQUIRED_GATES.csv"
ACCEPTANCE_GATES = f"{EXECUTION_PLAN}/ACCEPTANCE_GATES.csv"
TRACKER = f"{EXECUTION_PLAN}/TRACKER.csv"

REQUIRED_COVERAGE_TOKENS = [
    "lifecycle_operation_core",
    "lifecycle_state_transition_core",
    "lifecycle_invalid_transition_core",
    "lifecycle_route_core",
    "policy_override_no_override",
    "policy_override_create_database_only",
    "policy_override_security_admin",
    "policy_override_sysarch",
    "policy_override_policy_defined",
    "policy_override_cluster_only",
    "security_valid_credentials",
    "security_invalid_credentials",
    "auth_authority_engine_owned",
    "resource_seed_epoch_coverage",
    "diagnostic_message_vector_coverage",
    "observability_metrics_audit_coverage",
    "donor_mapping_firebird_sbsql",
    "sbsql_full_route_coverage",
    "mga_transaction_regression",
    "no_authoritative_wal_recovery",
    "no_parser_finality_authority",
    "no_donor_sql_execution",
    "evidence_report_present",
]

REQUIRED_LABEL_TOKENS = [
    "database_lifecycle_exhaustive",
    "database_lifecycle_fault_injection",
    "database_lifecycle_release",
    "DBLC_P16_REGRESSION_COMPLETE",
    "DBLC_STATIC_REGRESSION_REPORT_ARTIFACT",
    "database_lifecycle",
    "database_lifecycle_parser_route",
    "database_lifecycle_donor_mapping",
    "sbsql_parser_worker",
    "DBLC_P14_DONOR_MAPPING_COMPLETE",
    "DBLC_P15_OBSERVABILITY_COMPLETE",
]


def read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except FileNotFoundError:
        raise AssertionError(f"missing required file: {path}") from None


def required_lifecycle_labels(validation_plan: str) -> list[str]:
    head = validation_plan.split("## Gate Commands", 1)[0]
    labels = re.findall(r"\| `([^`]+)` \|", head)
    if not labels:
        raise AssertionError("VALIDATION_PLAN required CTest label table was not parsed")
    return labels


def source_test_metadata(repo_root: Path) -> str:
    parts: list[str] = []
    for cmake in (repo_root / "project/tests").rglob("CMakeLists.txt"):
        parts.append(read(cmake))
    return "\n".join(parts)


def matrix_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    required = {
        "coverage_id",
        "coverage_class",
        "required_surface",
        "evidence_labels",
        "evidence_artifacts",
        "status",
    }
    if not rows:
        raise AssertionError("regression matrix is empty")
    missing = required.difference(rows[0].keys())
    if missing:
        raise AssertionError(f"regression matrix missing columns: {sorted(missing)}")
    return rows


def require_contains(haystack: str, token: str, context: str, failures: list[str]) -> None:
    if token not in haystack:
        failures.append(f"{context}: missing `{token}`")


def main() -> int:
    parser = argparse.ArgumentParser(description="DBLC_STATIC_REGRESSION_REPORT_ARTIFACT")
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()

    repo_root = Path(args.repo_root).resolve()
    validation_plan = read(repo_root / VALIDATION_PLAN)
    report = read(repo_root / REGRESSION_REPORT)
    matrix_text = read(repo_root / REGRESSION_MATRIX)
    ctest_gates = read(repo_root / CTEST_REQUIRED_GATES)
    acceptance = read(repo_root / ACCEPTANCE_GATES)
    tracker = read(repo_root / TRACKER)
    cmake_metadata = source_test_metadata(repo_root)
    rows = matrix_rows(repo_root / REGRESSION_MATRIX)
    row_ids = {row["coverage_id"] for row in rows}
    row_statuses = {row["status"] for row in rows}

    failures: list[str] = []
    labels = required_lifecycle_labels(validation_plan)
    combined_artifacts = "\n".join([report, matrix_text, ctest_gates, acceptance, tracker])

    for token in REQUIRED_LABEL_TOKENS:
        require_contains(cmake_metadata, token, "CTest source metadata", failures)
        require_contains(combined_artifacts, token, "regression artifacts", failures)

    for token in REQUIRED_COVERAGE_TOKENS:
        require_contains(report, token, "regression report", failures)
        require_contains(matrix_text, token, "regression matrix", failures)

    for label in labels:
        require_contains(cmake_metadata, label, "CTest source metadata", failures)
        require_contains(report, label, "regression report", failures)
        expected_row = f"DBLC-P16-LABEL-{label}"
        if expected_row not in row_ids:
            failures.append(f"regression matrix missing row `{expected_row}`")

    for coverage in REQUIRED_COVERAGE_TOKENS:
        expected_prefix = f"DBLC-P16-"
        if not any(row_id.startswith(expected_prefix) and coverage in row_id for row_id in row_ids):
            failures.append(f"regression matrix missing coverage row for `{coverage}`")

    if row_statuses != {"covered"}:
        failures.append(f"regression matrix has non-covered statuses: {sorted(row_statuses)}")

    if "DBLC-016" not in tracker or "Exhaustive lifecycle regression suite" not in tracker:
        failures.append("TRACKER.csv does not retain the DBLC-016 regression-suite row")
    if "DBLC_P16_REGRESSION_COMPLETE" not in acceptance:
        failures.append("ACCEPTANCE_GATES.csv does not retain DBLC_P16_REGRESSION_COMPLETE")
    if "static:DBLC_STATIC_REGRESSION_REPORT_ARTIFACT" not in ctest_gates:
        failures.append("required CTest gate artifact does not list the DBLC-016 static gate")

    forbidden_overclaim = [
        "waived",
        "deferred",
        "todo",
        "placeholder",
        "future work",
    ]
    scan = "\n".join([report, matrix_text]).lower()
    for token in forbidden_overclaim:
        if token in scan:
            failures.append(f"regression artifacts contain forbidden overclaim token `{token}`")

    if failures:
        for failure in failures:
            print(f"DBLC-016 static audit failure: {failure}", file=sys.stderr)
        return 1

    print("DBLC_STATIC_REGRESSION_REPORT_ARTIFACT=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
