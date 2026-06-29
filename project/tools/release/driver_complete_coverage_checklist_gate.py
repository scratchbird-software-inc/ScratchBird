#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate per-driver comparison against the complete coverage checklist."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Any

from driver_complete_coverage_common import (
    COMPLETE_COVERAGE_REPORT,
    driver_rows,
    gate_range_present,
    index_rows,
    issue_status_summary,
    load_driver_manifest,
    load_matrix,
    native_tool_entries,
    report_status,
    source_path,
    write_report,
)
from driver_release_common import add_common_args, default_report_path, fail, resolve_repo_root


def build_report(repo_root: Path, matrix_path: Path) -> dict[str, Any]:
    manifest_rows = driver_rows(load_driver_manifest(repo_root))
    matrix_rows = load_matrix(matrix_path)
    manifest_by_component, manifest_index_issues = index_rows(manifest_rows, "component_id")
    matrix_by_component, matrix_index_issues = index_rows(matrix_rows, "component_id")
    tool_entries = native_tool_entries(repo_root)
    issues: list[str] = []
    issues.extend(f"manifest:{issue}" for issue in manifest_index_issues)
    issues.extend(f"matrix:{issue}" for issue in matrix_index_issues)

    expected = set(manifest_by_component)
    actual = set(matrix_by_component)
    for component in sorted(expected - actual):
        issues.append(f"matrix:{component}:missing_driver_row")
    for component in sorted(actual - expected):
        issues.append(f"matrix:{component}:not_a_manifest_driver")

    per_driver: list[dict[str, Any]] = []
    for component in sorted(expected & actual):
        manifest = manifest_by_component[component]
        matrix = matrix_by_component[component]
        name = manifest.get("name", "").strip()
        row_issues: list[str] = []
        if matrix.get("driver_name", "").strip() != name:
            row_issues.append("driver_name_mismatch")
        if matrix.get("source_path", "").strip() != manifest.get("source_path", "").strip():
            row_issues.append("source_path_mismatch")
        if not gate_range_present(matrix.get("required_gate_set", "")):
            row_issues.append("required_gate_set_missing_G1-G14_or_D1-D27")
        if not matrix.get("best_in_class_reference", "").strip():
            row_issues.append("missing_best_in_class_reference")
        if not matrix.get("checklist_source", "").strip():
            row_issues.append("missing_checklist_source")
        if matrix.get("comparison_status", "").strip() not in {
            "implemented_and_proven",
            "complete",
            "completed",
            "passed",
            "verified",
        }:
            row_issues.append(
                "comparison_status_non_closing:"
                + (matrix.get("comparison_status", "").strip() or "empty")
            )
        src = source_path(repo_root, manifest)
        if not src.is_dir():
            row_issues.append(f"source_path_missing:{manifest.get('source_path', '')}")
        tool = tool_entries.get(name)
        if tool is None:
            row_issues.append("missing_native_tool_matrix_entry")
        issues.extend(f"{component}:{issue}" for issue in row_issues)
        per_driver.append(
            {
                "component_id": component,
                "driver": name,
                "manifest_status": manifest.get("driver_status", ""),
                "release_bucket": manifest.get("release_bucket", ""),
                "comparison_status": matrix.get("comparison_status", ""),
                "source_path_exists": src.is_dir(),
                "baseline_mapping_exists": (src / "BASELINE_REQUIREMENT_MAPPING.md").is_file(),
                "native_tool_entry_exists": tool is not None,
                "native_tool_file_exists": bool(
                    tool and (repo_root / str(tool.get("path", ""))).exists()
                ),
                "issues": row_issues,
            }
        )

    return {
        "command": "driver_complete_coverage_checklist_gate.py",
        "gate_id": "BETA-DTA-GATE-032",
        "status": report_status(issues),
        "summary": {
            "manifest_driver_rows": len(manifest_rows),
            "matrix_rows": len(matrix_rows),
            "driver_rows_checked": len(per_driver),
            "status_summary": issue_status_summary(matrix_rows),
        },
        "drivers": per_driver,
        "issues": issues,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    add_common_args(parser, Path(__file__))
    parser.add_argument("--matrix", type=Path, required=True)
    args = parser.parse_args()
    repo_root = resolve_repo_root(args.repo_root)
    matrix_path = args.matrix.expanduser().resolve()
    output = args.output or default_report_path(repo_root, COMPLETE_COVERAGE_REPORT)
    try:
        report = build_report(repo_root, matrix_path)
    except (OSError, ValueError) as exc:
        return fail(str(exc))
    write_report(output, report)
    print(f"driver_complete_coverage_checklist={report['status']}")
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
