#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""DBLC-018 final zero-open execution_plan audit."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path
import sys


EXECUTION_PLAN_FILES = {
    "tracker": "TRACKER.csv",
    "acceptance": "ACCEPTANCE_GATES.csv",
    "audit": "SPEC_IMPLEMENTATION_AUDIT_MATRIX.csv",
    "gap": "artifacts/DATABASE_LIFECYCLE_IMPLEMENTATION_GAP_MATRIX.csv",
    "queue": "artifacts/DATABASE_LIFECYCLE_SLICE_EXECUTION_QUEUE.csv",
    "agent_status": "artifacts/DATABASE_LIFECYCLE_AGENT_STATUS.csv",
    "write_scope": "artifacts/DATABASE_LIFECYCLE_AGENT_WRITE_SCOPE_REGISTER.csv",
    "ctest_gates": "artifacts/DATABASE_LIFECYCLE_CTEST_REQUIRED_GATES.csv",
}

FINAL_ARTIFACTS = (
    "FINAL_AUDIT.md",
    "VALIDATION_RESULT.md",
    "CLOSURE_REPORT.md",
    "artifacts/final_database_lifecycle_zero_open_audit.py",
)

BAD_STATUS_TOKENS = (
    "pending",
    "blocked",
    "partial",
    "not_started",
    "not started",
    "required",
    "gap",
)

ALLOWED_BAD_CONTEXT = (
    "not_required",
    "not_applicable",
    "no gap",
    "zero-open",
    "zero open",
    "required_commands",
    "required DBLC",
    "required lifecycle labels pass",
    "required ipc lifecycle labels pass",
)


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except FileNotFoundError:
        raise AssertionError(f"missing required file: {path}") from None


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    if not rows:
        raise AssertionError(f"empty CSV: {path}")
    return rows


def bad_status(value: str) -> bool:
    lowered = value.lower()
    if any(token in lowered for token in ALLOWED_BAD_CONTEXT):
        return False
    return any(token in lowered for token in BAD_STATUS_TOKENS)


def require_all(rows: list[dict[str, str]], column: str, expected: str, label: str) -> list[str]:
    failures: list[str] = []
    for row in rows:
        if row.get(column, "") != expected:
            failures.append(f"{label}:{row.get('slice_id') or row.get('gate_id') or row.get('source_search_key')}: {column}={row.get(column, '')!r}")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture-root", "--execution_plan-root", dest="execution_plan_root", type=Path)
    args = parser.parse_args()

    execution_plan_root = args.execution_plan_root
    if execution_plan_root is None:
        execution_plan_root = Path(__file__).resolve().parents[1]
    execution_plan_root = execution_plan_root.resolve()

    failures: list[str] = []
    paths = {name: execution_plan_root / rel for name, rel in EXECUTION_PLAN_FILES.items()}

    tracker = read_csv(paths["tracker"])
    acceptance = read_csv(paths["acceptance"])
    audit = read_csv(paths["audit"])
    gap = read_csv(paths["gap"])
    queue = read_csv(paths["queue"])
    agent_status = read_csv(paths["agent_status"])
    write_scope = read_csv(paths["write_scope"])
    ctest_gates_text = read_text(paths["ctest_gates"])

    failures.extend(require_all(tracker, "status", "passed", "TRACKER.csv"))
    failures.extend(require_all(acceptance, "status", "passed", "ACCEPTANCE_GATES.csv"))
    failures.extend(require_all(audit, "status", "passed", "SPEC_IMPLEMENTATION_AUDIT_MATRIX.csv"))
    failures.extend(require_all(queue, "status", "validation_passed", "DATABASE_LIFECYCLE_SLICE_EXECUTION_QUEUE.csv"))

    for row in queue:
        if row.get("last_validation_result") != "passed" and not row.get("last_validation_result", "").startswith("passed:"):
            failures.append(f"queue:{row.get('slice_id')}: last_validation_result={row.get('last_validation_result')!r}")

    for row in gap:
        for column in (
            "canonical_spec_status",
            "registry_status",
            "engine_api_status",
            "server_ipc_parser_status",
            "test_status",
            "diagnostic_status",
            "current_implementation_status",
            "gap_summary",
        ):
            value = row.get(column, "")
            if bad_status(value):
                failures.append(f"gap:{row.get('slice_id')}:{column}={value!r}")

    for row in agent_status:
        status = row.get("status", "")
        if status not in {"completed_scope_released", "validation_passed", "released", "closed"}:
            failures.append(f"agent_status:{row.get('slice_id')}:{row.get('agent_id')}: status={status!r}")

    for row in write_scope:
        if row.get("status") != "released":
            failures.append(f"write_scope:{row.get('scope_id')}: status={row.get('status')!r}")
        if not row.get("released_utc"):
            failures.append(f"write_scope:{row.get('scope_id')}: released_utc missing")

    for rel in FINAL_ARTIFACTS:
        text = read_text(execution_plan_root / rel)
        if "DBLC_P18_FINAL_CLEAN" not in text and rel != "artifacts/final_database_lifecycle_zero_open_audit.py":
            failures.append(f"{rel}: missing DBLC_P18_FINAL_CLEAN marker")

    if "database_lifecycle_release" not in ctest_gates_text:
        failures.append("DATABASE_LIFECYCLE_CTEST_REQUIRED_GATES.csv missing database_lifecycle_release")
    if "DBLC_STATIC_FINAL_ZERO_OPEN_AUDIT" not in ctest_gates_text:
        failures.append("DATABASE_LIFECYCLE_CTEST_REQUIRED_GATES.csv missing DBLC_STATIC_FINAL_ZERO_OPEN_AUDIT")

    repo_root = execution_plan_root
    while repo_root != repo_root.parent:
        if (repo_root / "project/tests/database_lifecycle/CMakeLists.txt").exists():
            break
        repo_root = repo_root.parent
    else:
        failures.append("repository root could not be located from fixture root")
        repo_root = execution_plan_root
    cmake = read_text(repo_root / "project/tests/database_lifecycle/CMakeLists.txt")
    for token in (
        "database_lifecycle_release_zero_open_audit",
        "database_lifecycle_release",
        "DBLC_STATIC_FINAL_ZERO_OPEN_AUDIT",
        "DBLC_P18_FINAL_CLEAN",
    ):
        if token not in cmake:
            failures.append(f"CMakeLists.txt missing {token}")

    readme = read_text(execution_plan_root / "README.md")
    validation_plan = read_text(execution_plan_root / "VALIDATION_PLAN.md")
    if "Status: passed" not in readme:
        failures.append("README.md status is not passed")
    if "Status: passed" not in validation_plan:
        failures.append("VALIDATION_PLAN.md status is not passed")

    if failures:
        for failure in failures:
            print(f"DBLC_STATIC_FINAL_ZERO_OPEN_AUDIT=failed: {failure}", file=sys.stderr)
        return 1

    print("DBLC_STATIC_FINAL_ZERO_OPEN_AUDIT=passed")
    print("DBLC_P18_FINAL_CLEAN=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
