#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Fail beta driver release when any in-scope lane or gate remains deferred."""

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
    load_workplan_csv,
    report_status,
    resolve_repo_root,
    resolve_workplan_root,
    status_value,
    write_report,
)


REPORT_NAME = "driver_beta_no_deferral.json"

STATUS_FILES = (
    ("LANE_COMPLETION_MATRIX.csv", "component_id"),
    ("TRACKER.csv", "slice_id"),
    ("ACCEPTANCE_GATES.csv", "gate_id"),
)


def row_is_excluded(filename: str, row: dict[str, str]) -> bool:
    if filename == "LANE_COMPLETION_MATRIX.csv":
        return row.get("component_id", "").strip() == DBEAVER_COMPONENT_ID
    return False


def check_status_files(workplan_root: Path) -> tuple[list[str], dict[str, Any]]:
    issues: list[str] = []
    summary: dict[str, Any] = {}
    for filename, id_field in STATUS_FILES:
        rows = load_workplan_csv(workplan_root, filename)
        checked = 0
        blocked = 0
        for row in rows:
            if row_is_excluded(filename, row):
                continue
            checked += 1
            status = status_value(row)
            if not is_closing_status(status):
                blocked += 1
                issues.append(
                    f"{filename}:{row.get(id_field, '').strip()}:non_closing_status:{status or 'empty'}"
                )
        summary[filename] = {"checked": checked, "blocked": blocked}
    return issues, summary


def build_report(workplan_root: Path) -> dict[str, Any]:
    issues, summary = check_status_files(workplan_root)
    return {
        "command": "driver_beta_no_deferral_gate.py",
        "gate_id": "BETA-DTA-GATE-002",
        "status": report_status(issues),
        "summary": summary,
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
        report = build_report(workplan_root)
    except (OSError, ValueError) as exc:
        return fail(str(exc))
    write_report(output, report)
    print(f"driver_beta_no_deferral={report['status']}")
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
