#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Manager gate for driver-first complete coverage closure."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

from driver_complete_coverage_common import (
    MANAGER_CLOSURE_REPORT,
    MATRIX_STATUS_FIELDS,
    all_matrix_statuses_closing,
    index_rows,
    issue_status_summary,
    load_matrix,
    report_status,
    write_report,
)
from driver_release_common import (
    add_common_args,
    default_report_path,
    fail,
    is_closing_status,
    load_workplan_csv,
    resolve_repo_root,
    resolve_workplan_root,
    status_value,
)


REQUIRED_REPORTS = (
    "driver_complete_coverage_checklist.json",
    "driver_complete_delta_implementation.json",
    "driver_complete_coverage_tests.json",
    "driver_wiki_documentation.json",
    "driver_packaging_promotion.json",
)


def read_report(path: Path) -> dict[str, Any] | None:
    if not path.is_file():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return {"status": "fail", "issues": [f"invalid_json:{path}"]}


def build_report(repo_root: Path, workplan_root: Path) -> dict[str, Any]:
    matrix_rows = load_matrix(workplan_root / "DRIVER_COMPLETE_COVERAGE_CHECKLIST_MATRIX.csv")
    lane_rows = load_workplan_csv(workplan_root, "LANE_COMPLETION_MATRIX.csv")
    gate_rows = load_workplan_csv(workplan_root, "ACCEPTANCE_GATES.csv")
    lane_by_component, lane_index_issues = index_rows(lane_rows, "component_id")
    issues: list[str] = [f"lane_matrix:{issue}" for issue in lane_index_issues]

    for row in matrix_rows:
        component = row.get("component_id", "").strip()
        if not all_matrix_statuses_closing(row):
            open_fields = [
                f"{field}={row.get(field, '') or 'empty'}"
                for field in MATRIX_STATUS_FIELDS
                if not is_closing_status(row.get(field, ""))
            ]
            issues.append(f"driver_complete_matrix:{component}:open_fields:{';'.join(open_fields)}")
        lane = lane_by_component.get(component)
        if lane is None:
            issues.append(f"lane_matrix:{component}:missing_lane")
        elif not is_closing_status(status_value(lane)):
            issues.append(
                f"lane_matrix:{component}:non_closing_status:{status_value(lane) or 'empty'}"
            )

    for gate_id in ("BETA-DTA-GATE-032", "BETA-DTA-GATE-033", "BETA-DTA-GATE-034", "BETA-DTA-GATE-035", "BETA-DTA-GATE-036"):
        matches = [row for row in gate_rows if row.get("gate_id", "").strip() == gate_id]
        if not matches:
            issues.append(f"acceptance_gate:{gate_id}:missing")
            continue
        status = matches[0].get("status", "").strip()
        if not is_closing_status(status):
            issues.append(f"acceptance_gate:{gate_id}:non_closing_status:{status or 'empty'}")

    report_statuses: dict[str, str] = {}
    reports_root = repo_root / "build" / "reports"
    for name in REQUIRED_REPORTS:
        report = read_report(reports_root / name)
        if report is None:
            issues.append(f"proof_report:missing:{name}")
            report_statuses[name] = "missing"
            continue
        status = str(report.get("status", "")).strip()
        report_statuses[name] = status or "empty"
        if status != "pass":
            issues.append(f"proof_report:{name}:status:{status or 'empty'}")

    return {
        "command": "driver_first_manager_closure_gate.py",
        "gate_id": "BETA-DTA-GATE-037",
        "status": report_status(issues),
        "summary": {
            "driver_rows": len(matrix_rows),
            "status_summary": issue_status_summary(matrix_rows),
            "proof_report_statuses": report_statuses,
        },
        "issues": issues,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    add_common_args(parser, Path(__file__))
    args = parser.parse_args()
    repo_root = resolve_repo_root(args.repo_root)
    workplan_root = resolve_workplan_root(repo_root, args.workplan_root)
    output = args.output or default_report_path(repo_root, MANAGER_CLOSURE_REPORT)
    try:
        report = build_report(repo_root, workplan_root)
    except (OSError, ValueError) as exc:
        return fail(str(exc))
    write_report(output, report)
    print(f"driver_first_manager_closure={report['status']}")
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
