#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate implementation closure for complete-coverage driver deltas."""

from __future__ import annotations

import argparse
import re
from pathlib import Path
from typing import Any

from driver_complete_coverage_common import (
    DELTA_IMPLEMENTATION_REPORT,
    RELEASE_BUCKETS,
    baseline_mapping_path,
    closing_matrix_rows,
    driver_rows,
    index_rows,
    issue_status_summary,
    load_driver_manifest,
    load_matrix,
    native_tool_entries,
    package_contract_path,
    report_status,
    source_path,
    write_report,
)
from driver_release_common import add_common_args, default_report_path, fail, resolve_repo_root


REQUIRED_STATUS_FIELDS = (
    "comparison_status",
    "delta_workplan_status",
    "implementation_status",
)

REQUIRED_BASELINE_MARKERS = (
    "CONN",
    "TXN",
    "EXEC",
    "META",
    "TYPE",
    "ERR",
    "RES",
)

TRACEABILITY_REL = Path("project/drivers/driver/COMPLETE_COVERAGE_TRACEABILITY.md")
STRICT_FORBIDDEN_IMPLEMENTATION_TEXT = (
    "not implemented",
    "not yet implemented",
    "contract_only",
    "contract only",
    "fake success",
)
CONTEXTUAL_FORBIDDEN_IMPLEMENTATION_TEXT = ("placeholder", "stub")
FORBIDDEN_IMPLEMENTATION_TEXT = (
    *STRICT_FORBIDDEN_IMPLEMENTATION_TEXT,
    *CONTEXTUAL_FORBIDDEN_IMPLEMENTATION_TEXT,
)
GENERATED_DIR_NAMES = {
    ".build",
    ".dart_tool",
    ".gradle",
    ".pytest_cache",
    "__pycache__",
    "bin",
    "build",
    "dist",
    "node_modules",
    "obj",
    "target",
}
TEST_DIR_NAMES = {"t", "test", "tests"}
TEXT_FILE_SUFFIXES = {
    "",
    ".c",
    ".cc",
    ".cpp",
    ".cs",
    ".dart",
    ".ex",
    ".exs",
    ".go",
    ".java",
    ".jl",
    ".js",
    ".json",
    ".md",
    ".mojo",
    ".pas",
    ".php",
    ".pl",
    ".pm",
    ".py",
    ".r",
    ".rb",
    ".rs",
    ".swift",
    ".toml",
    ".ts",
    ".yaml",
    ".yml",
}


def forbidden_implementation_token(line: str) -> str | None:
    lowered = line.lower()
    for token in STRICT_FORBIDDEN_IMPLEMENTATION_TEXT:
        if token in lowered:
            return token
    if re.search(r"\bplaceholder\b", lowered) and any(
        marker in lowered
        for marker in (
            "todo",
            "temporary",
            "implementation",
            "runtime",
            "fake success",
            "return success",
            "placeholder only",
        )
    ):
        return "placeholder"
    if re.search(r"\bstub\b", lowered) and any(
        marker in lowered
        for marker in (
            "todo",
            "temporary",
            "implementation",
            "runtime",
            "fake success",
            "return success",
            "stub only",
        )
    ):
        return "stub"
    return None


def build_report(repo_root: Path, matrix_path: Path) -> dict[str, Any]:
    manifest_rows = driver_rows(load_driver_manifest(repo_root))
    matrix_rows = load_matrix(matrix_path)
    manifest_by_component, manifest_index_issues = index_rows(manifest_rows, "component_id")
    matrix_by_component, matrix_index_issues = index_rows(matrix_rows, "component_id")
    tool_entries = native_tool_entries(repo_root)
    traceability = repo_root / TRACEABILITY_REL
    traceability_text = (
        traceability.read_text(encoding="utf-8", errors="replace").upper()
        if traceability.is_file()
        else ""
    )
    issues: list[str] = []
    issues.extend(f"manifest:{issue}" for issue in manifest_index_issues)
    issues.extend(f"matrix:{issue}" for issue in matrix_index_issues)
    issues.extend(closing_matrix_rows(matrix_rows, REQUIRED_STATUS_FIELDS))

    per_driver: list[dict[str, Any]] = []
    for component in sorted(set(manifest_by_component) & set(matrix_by_component)):
        manifest = manifest_by_component[component]
        matrix = matrix_by_component[component]
        name = manifest.get("name", "").strip()
        row_issues: list[str] = []
        if manifest.get("driver_status", "").strip() == "planned_not_implemented":
            row_issues.append("driver_manifest_still_planned_not_implemented")
        if manifest.get("release_bucket", "").strip() not in RELEASE_BUCKETS:
            row_issues.append(
                "driver_release_bucket_not_release_candidate:"
                + (manifest.get("release_bucket", "").strip() or "empty")
            )
        src = source_path(repo_root, manifest)
        if not src.is_dir():
            row_issues.append("source_path_missing")
        baseline = baseline_mapping_path(repo_root, manifest)
        marker_hits: list[str] = []
        if baseline.is_file():
            text = baseline.read_text(encoding="utf-8", errors="replace")
            upper = text.upper()
            marker_hits = [marker for marker in REQUIRED_BASELINE_MARKERS if marker in upper]
            for marker in REQUIRED_BASELINE_MARKERS:
                if marker not in upper:
                    row_issues.append(f"baseline_mapping_missing_marker:{marker}")
            combined_traceability = upper + "\n" + traceability_text
            if "G1" not in combined_traceability or "G14" not in combined_traceability or "D27" not in combined_traceability:
                row_issues.append("baseline_mapping_missing_complete_checklist_traceability")
        else:
            row_issues.append("missing_BASELINE_REQUIREMENT_MAPPING")
        contract = package_contract_path(repo_root, manifest)
        if contract.is_file():
            contract_text = contract.read_text(encoding="utf-8", errors="replace").lower()
            for token in ("contract_only", "fail_closed_runtime", "implementation_limits"):
                if token in contract_text:
                    row_issues.append(f"package_contract_open_status_token:{token}")
        if src.is_dir():
            for path in sorted(src.rglob("*")):
                if any(part in GENERATED_DIR_NAMES for part in path.parts):
                    continue
                if any(part in TEST_DIR_NAMES for part in path.parts):
                    continue
                if not path.is_file() or path.suffix.lower() not in TEXT_FILE_SUFFIXES:
                    continue
                try:
                    text = path.read_text(encoding="utf-8", errors="replace")
                except OSError:
                    continue
                for line in text.splitlines():
                    token = forbidden_implementation_token(line)
                    if token is not None:
                        try:
                            rel = path.relative_to(repo_root)
                        except ValueError:
                            rel = path
                        row_issues.append(f"open_implementation_text:{rel}:{token}")
                        break
        tool = tool_entries.get(name)
        if tool is None:
            row_issues.append("missing_native_tool_entry")
        else:
            tool_path = repo_root / str(tool.get("path", ""))
            if not tool_path.exists():
                row_issues.append("native_tool_file_missing")
        for field in REQUIRED_STATUS_FIELDS:
            if matrix.get(field, "").strip() == "not_applicable_with_citation":
                row_issues.append(f"{field}:not_applicable_not_valid_for_whole_driver_lane")
        issues.extend(f"{component}:{issue}" for issue in row_issues)
        per_driver.append(
            {
                "component_id": component,
                "driver": name,
                "driver_status": manifest.get("driver_status", ""),
                "release_bucket": manifest.get("release_bucket", ""),
                "matrix_statuses": {field: matrix.get(field, "") for field in REQUIRED_STATUS_FIELDS},
                "baseline_markers": marker_hits,
                "issues": row_issues,
            }
        )

    for component in sorted(set(manifest_by_component) - set(matrix_by_component)):
        issues.append(f"{component}:missing_matrix_row")

    return {
        "command": "driver_complete_delta_implementation_gate.py",
        "gate_id": "BETA-DTA-GATE-033",
        "status": report_status(issues),
        "summary": {
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
    output = args.output or default_report_path(repo_root, DELTA_IMPLEMENTATION_REPORT)
    try:
        report = build_report(repo_root, args.matrix.expanduser().resolve())
    except (OSError, ValueError) as exc:
        return fail(str(exc))
    write_report(output, report)
    print(f"driver_complete_delta_implementation={report['status']}")
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
