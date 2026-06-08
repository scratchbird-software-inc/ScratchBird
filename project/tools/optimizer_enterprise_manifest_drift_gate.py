#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""OEIC optimizer execution_plan and implementation-anchor drift gate.

SEARCH_KEY: OEIC_OPTIMIZER_GENERATED_MANIFEST_DRIFT_GATE

This gate intentionally checks the execution_plan CSVs, implementation anchors, and
CTest wiring from repository state. It is not a marketing artifact; it fails
when a completed OEIC row lacks a matching completed acceptance/audit row or
when an audit search key cannot be found in its referenced implementation file.
"""

from __future__ import annotations

import argparse
import csv
import pathlib
import re
import sys
from typing import Iterable


EXPECTED_COMPLETED = {f"OEIC-{index:03d}" for index in range(0, 5)}
EXPECTED_COMPLETED.update({f"OEIC-{index:03d}" for index in range(10, 18)})
EXPECTED_COMPLETED.update({f"OEIC-{index:03d}" for index in range(20, 23)})
EXPECTED_COMPLETED.update({f"OEIC-{index:03d}" for index in range(30, 33)})
EXPECTED_COMPLETED.update({f"OEIC-{index:03d}" for index in range(40, 44)})
EXPECTED_COMPLETED.update({f"OEIC-{index:03d}" for index in range(50, 53)})
EXPECTED_COMPLETED.update({f"OEIC-{index:03d}" for index in range(60, 64)})
EXPECTED_COMPLETED.update({f"OEIC-{index:03d}" for index in range(70, 73)})
EXPECTED_COMPLETED.update({f"OEIC-{index:03d}" for index in range(80, 86)})
EXPECTED_COMPLETED.update({"OEIC-090", "OEIC-091"})
EXPECTED_COMPLETED.add("OEIC-100")

REQUIRED_OPTIMIZER_TEST_TOKENS = (
    "optimizer_enterprise_route_validation_gate",
    "optimizer_enterprise_maintainability_gate",
    "optimizer_enterprise_manifest_drift_gate",
)

REQUIRED_PROJECT_TEST_TOKENS = (
    "optimizer_production_build_cmake_gate",
    "optimizer_production_build_configure_gate",
)


def read_csv(path: pathlib.Path) -> list[dict[str, str]]:
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def normalize_audit_id(audit_id: str) -> str:
    return audit_id.replace("OEIC-AUDIT-", "OEIC-")


def normalize_gate_id(gate_id: str) -> str:
    return gate_id.replace("OEIC-GATE-", "OEIC-")


def resolve_anchor_path(repo: pathlib.Path,
                        execution_plan: pathlib.Path,
                        anchor_path: str) -> pathlib.Path:
    if anchor_path.endswith(" README"):
        anchor_path = anchor_path[:-len(" README")] + "/README.md"
    if anchor_path.startswith("/"):
        return pathlib.Path(anchor_path)
    if anchor_path.startswith("artifacts/"):
        return execution_plan / anchor_path
    resolved = repo / anchor_path
    if resolved.exists():
        return resolved
    active_prefix = "docs" "/execution-plans/optimizer-enterprise-implementation-closure"
    completed_prefix = \
        "docs" "/completed-execution-plans/optimizer-enterprise-implementation-closure"
    if anchor_path.startswith(active_prefix):
        completed = repo / (completed_prefix + anchor_path[len(active_prefix):])
        if completed.exists():
            return completed
    return resolved


def resolve_execution_plan(repo: pathlib.Path) -> pathlib.Path:
    for root in ("execution-plans", "completed-execution-plans"):
        candidate = repo / "docs" / root / \
            "optimizer-enterprise-implementation-closure"
        if candidate.exists():
            return candidate
    fail("optimizer-enterprise-implementation-closure execution_plan package missing")


def parse_anchor(anchor: str) -> tuple[str, str] | None:
    match = re.match(r"(.+?) search key ([A-Za-z0-9_.:-]+)$", anchor.strip())
    if not match:
        return None
    return match.group(1), match.group(2)


def iter_anchor_entries(anchor_field: str) -> Iterable[str]:
    for raw in anchor_field.split(";"):
        entry = raw.strip()
        if entry:
            yield entry


def validate_completed_status(rows: list[dict[str, str]],
                              key_name: str,
                              normalizer,
                              file_name: str) -> None:
    by_id = {normalizer(row[key_name]): row for row in rows}
    for row_id in sorted(EXPECTED_COMPLETED):
      row = by_id.get(row_id)
      if row is None:
          fail(f"{file_name}: missing {row_id}")
      if row.get("status") != "completed":
          fail(f"{file_name}: {row_id} is {row.get('status')} not completed")


def validate_audit_anchors(repo: pathlib.Path,
                           execution_plan: pathlib.Path,
                           audit_rows: list[dict[str, str]]) -> None:
    for row in audit_rows:
        row_id = normalize_audit_id(row["audit_id"])
        if row_id not in EXPECTED_COMPLETED:
            continue
        if row.get("status") != "completed":
            fail(f"audit matrix: {row_id} is not completed")
        saw_search_key = False
        for anchor in iter_anchor_entries(row["anchor"]):
            parsed = parse_anchor(anchor)
            if parsed is None:
                continue
            rel_path, search_key = parsed
            file_path = resolve_anchor_path(repo, execution_plan, rel_path)
            if not file_path.exists():
                fail(f"{row_id}: anchor file missing: {file_path}")
            text = file_path.read_text(errors="replace")
            if search_key not in text:
                fail(f"{row_id}: search key {search_key} missing in {file_path}")
            saw_search_key = True
        if not saw_search_key:
            fail(f"{row_id}: no searchable implementation anchor")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True)
    args = parser.parse_args()

    repo = pathlib.Path(args.repo).resolve()
    project_root = repo / "project" if (repo / "project").is_dir() else repo
    execution_plan = resolve_execution_plan(repo)
    tracker = read_csv(execution_plan / "TRACKER.csv")
    gates = read_csv(execution_plan / "ACCEPTANCE_GATES.csv")
    audits = read_csv(execution_plan / "SPEC_IMPLEMENTATION_AUDIT_MATRIX.csv")

    validate_completed_status(tracker, "slice_id", lambda value: value,
                              "TRACKER.csv")
    validate_completed_status(gates, "gate_id", normalize_gate_id,
                              "ACCEPTANCE_GATES.csv")
    validate_completed_status(audits, "audit_id", normalize_audit_id,
                              "SPEC_IMPLEMENTATION_AUDIT_MATRIX.csv")
    validate_audit_anchors(repo, execution_plan, audits)

    tests_cmake = project_root / "tests" / "optimizer" / "CMakeLists.txt"
    cmake_text = tests_cmake.read_text(errors="replace")
    for token in REQUIRED_OPTIMIZER_TEST_TOKENS:
        if token not in cmake_text:
            fail(f"tests CMake missing {token}")
    project_cmake_text = (project_root / "CMakeLists.txt").read_text(
        errors="replace")
    for token in REQUIRED_PROJECT_TEST_TOKENS:
        if token not in project_cmake_text:
            fail(f"project CMake missing {token}")

    optimizer_cmake = project_root / "src" / "engine" / "optimizer" / \
        "CMakeLists.txt"
    optimizer_cmake_text = optimizer_cmake.read_text(errors="replace")
    if "runtime_consumption_benchmark_evidence.cpp" not in optimizer_cmake_text:
        fail("optimizer target missing runtime_consumption_benchmark_evidence.cpp")

    print("OEIC optimizer manifest drift gate passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
