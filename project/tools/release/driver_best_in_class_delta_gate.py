#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Verify best-in-class delta closure for every in-scope beta lane."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Any

from driver_release_common import (
    DBEAVER_COMPONENT_ID,
    add_common_args,
    default_report_path,
    fail,
    is_closing_status,
    load_manifest,
    load_workplan_csv,
    report_status,
    resolve_repo_root,
    resolve_workplan_root,
    status_value,
    unique_index,
    write_report,
)


REPORT_NAME = "driver_best_in_class_delta.json"
BEST_IN_CLASS_GATE = "BETA-DTA-GATE-010"


def build_report(repo_root: Path, workplan_root: Path) -> dict[str, Any]:
    manifest_rows = load_manifest(repo_root)
    lane_rows = load_workplan_csv(workplan_root, "LANE_COMPLETION_MATRIX.csv")
    capability_rows = load_workplan_csv(workplan_root, "DRIVER_CAPABILITY_BASELINE_MATRIX.csv")
    capability_by_group = {
        row.get("capability_group", "").strip(): row for row in capability_rows
    }
    lane_by_id, index_issues = unique_index(lane_rows, "component_id", "LANE_COMPLETION_MATRIX")
    issues = list(index_issues)

    best_row = capability_by_group.get("best_in_class_delta")
    if best_row is None:
        issues.append("capability_matrix:missing_best_in_class_delta")
    elif not is_closing_status(status_value(best_row)):
        issues.append(
            "capability_matrix:best_in_class_delta:non_closing_status:"
            f"{status_value(best_row) or 'empty'}"
        )

    checked = 0
    for row in manifest_rows:
        component = row.get("component_id", "").strip()
        if component == DBEAVER_COMPONENT_ID:
            continue
        checked += 1
        lane = lane_by_id.get(component)
        if lane is None:
            issues.append(f"lane_matrix:{component}:missing_lane_row")
            continue
        if BEST_IN_CLASS_GATE not in lane.get("required_shared_gates", "").split(";"):
            issues.append(f"lane_matrix:{component}:missing_required_gate:{BEST_IN_CLASS_GATE}")
        if not is_closing_status(status_value(lane)):
            issues.append(
                f"lane_matrix:{component}:non_closing_status:{status_value(lane) or 'empty'}"
            )
    return {
        "command": "driver_best_in_class_delta_gate.py",
        "gate_id": BEST_IN_CLASS_GATE,
        "status": report_status(issues),
        "summary": {
            "components_checked": checked,
            "capability_rows": len(capability_rows),
        },
        "issues": issues,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    add_common_args(parser, Path(__file__))
    args = parser.parse_args()
    repo_root = resolve_repo_root(args.repo_root)
    workplan_root = resolve_workplan_root(repo_root, args.workplan_root)
    output = args.output or default_report_path(repo_root, REPORT_NAME)
    try:
        report = build_report(repo_root, workplan_root)
    except (OSError, ValueError) as exc:
        return fail(str(exc))
    write_report(output, report)
    print(f"driver_best_in_class_delta={report['status']}")
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
