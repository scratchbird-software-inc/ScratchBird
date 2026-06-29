#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Shared helpers for driver complete-coverage release gates."""

from __future__ import annotations

import csv
import json
from pathlib import Path
from typing import Any

from driver_release_common import is_closing_status, status_value


DRIVER_MANIFEST_REL = Path("project/drivers/DriverPackageManifest.csv")
NATIVE_TOOL_MATRIX_REL = Path("project/tests/conformance/drivers/native_tool_matrix.json")
COMPLETE_COVERAGE_REPORT = "driver_complete_coverage_checklist.json"
DELTA_IMPLEMENTATION_REPORT = "driver_complete_delta_implementation.json"
COMPLETE_COVERAGE_TEST_REPORT = "driver_complete_coverage_tests.json"
WIKI_DOCUMENTATION_REPORT = "driver_wiki_documentation.json"
PACKAGING_PROMOTION_REPORT = "driver_packaging_promotion.json"
MANAGER_CLOSURE_REPORT = "driver_first_manager_closure.json"

REQUIRED_GATE_RANGE_TOKENS = ("G1-G14", "D1-D27")
REQUIRED_GATE_IDS = {f"G{index}" for index in range(1, 15)}
REQUIRED_REGISTRY_PREFIXES = {f"D{index}" for index in range(1, 28)}
RELEASE_BUCKETS = {"release_candidate", "release_supported", "supported"}
PLANNED_NOT_IMPLEMENTED = {"planned_not_implemented"}

MATRIX_STATUS_FIELDS = (
    "comparison_status",
    "delta_workplan_status",
    "implementation_status",
    "test_status",
    "wiki_doc_status",
    "packaging_status",
)


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8-sig") as handle:
        return list(csv.DictReader(handle))


def read_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def load_driver_manifest(repo_root: Path) -> list[dict[str, str]]:
    return read_csv(repo_root / DRIVER_MANIFEST_REL)


def load_matrix(path: Path) -> list[dict[str, str]]:
    return read_csv(path)


def driver_rows(rows: list[dict[str, str]]) -> list[dict[str, str]]:
    return [row for row in rows if row.get("category", "").strip() == "driver"]


def index_rows(rows: list[dict[str, str]], field: str) -> tuple[dict[str, dict[str, str]], list[str]]:
    index: dict[str, dict[str, str]] = {}
    issues: list[str] = []
    for row in rows:
        key = row.get(field, "").strip()
        if not key:
            issues.append(f"{field}:empty_key")
            continue
        if key in index:
            issues.append(f"{field}:duplicate:{key}")
            continue
        index[key] = row
    return index, issues


def native_tool_entries(repo_root: Path) -> dict[str, dict[str, Any]]:
    path = repo_root / NATIVE_TOOL_MATRIX_REL
    if not path.is_file():
        return {}
    doc = read_json(path)
    entries: dict[str, dict[str, Any]] = {}
    for item in doc.get("driver_tools", []):
        if isinstance(item, dict):
            driver = str(item.get("driver", "")).strip()
            if driver:
                entries[driver] = item
    return entries


def source_path(repo_root: Path, row: dict[str, str]) -> Path:
    return repo_root / row.get("source_path", "").strip()


def baseline_mapping_path(repo_root: Path, row: dict[str, str]) -> Path:
    return source_path(repo_root, row) / "BASELINE_REQUIREMENT_MAPPING.md"


def package_contract_path(repo_root: Path, row: dict[str, str]) -> Path:
    return source_path(repo_root, row) / "package_contract.json"


def is_release_ready_manifest(row: dict[str, str]) -> bool:
    return (
        row.get("driver_status", "").strip() not in PLANNED_NOT_IMPLEMENTED
        and row.get("release_bucket", "").strip() in RELEASE_BUCKETS
    )


def matrix_status_complete(row: dict[str, str], field: str) -> bool:
    return is_closing_status(row.get(field, ""))


def status_issue(row: dict[str, str], field: str) -> str | None:
    if matrix_status_complete(row, field):
        return None
    return f"{row.get('component_id', '<unknown>')}:{field}:non_closing:{row.get(field, '') or 'empty'}"


def write_report(path: Path, report: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def report_status(issues: list[str]) -> str:
    return "fail" if issues else "pass"


def issue_status_summary(rows: list[dict[str, str]]) -> dict[str, dict[str, int]]:
    summary: dict[str, dict[str, int]] = {}
    for field in MATRIX_STATUS_FIELDS:
        counts: dict[str, int] = {}
        for row in rows:
            value = row.get(field, "").strip() or "empty"
            counts[value] = counts.get(value, 0) + 1
        summary[field] = counts
    return summary


def gate_range_present(value: str) -> bool:
    normalized = value.replace(" ", "")
    return all(token in normalized for token in REQUIRED_GATE_RANGE_TOKENS)


def closing_matrix_rows(rows: list[dict[str, str]], fields: tuple[str, ...]) -> list[str]:
    issues: list[str] = []
    for row in rows:
        for field in fields:
            issue = status_issue(row, field)
            if issue is not None:
                issues.append(issue)
    return issues


def all_matrix_statuses_closing(row: dict[str, str]) -> bool:
    return all(matrix_status_complete(row, field) for field in MATRIX_STATUS_FIELDS)


def closing_text(value: str) -> str:
    return "closed" if is_closing_status(value) else f"open:{status_value({'status': value}) or value or 'empty'}"
