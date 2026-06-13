#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Probe beta driver lane toolchain availability from the workplan matrix."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
from typing import Any

from driver_release_common import (
    DBEAVER_COMPONENT_ID,
    add_common_args,
    command_exists,
    command_version,
    commands_for_toolchain,
    default_report_path,
    fail,
    lane_tokens,
    load_manifest,
    load_workplan_csv,
    report_status,
    resolve_repo_root,
    resolve_workplan_root,
    status_value,
    unique_index,
    write_report,
)


REPORT_NAME = "driver_toolchain_probe.json"


def component_is_covered(component: dict[str, str], matrix_tokens: set[str]) -> bool:
    return (
        component.get("name", "").strip() in matrix_tokens
        or component.get("driver_family", "").strip() in matrix_tokens
    )


def build_report(
    repo_root: Path,
    workplan_root: Path,
    allow_missing: bool,
) -> dict[str, Any]:
    manifest_rows = load_manifest(repo_root)
    matrix_rows = load_workplan_csv(workplan_root, "PLATFORM_TOOLCHAIN_MATRIX.csv")
    _, index_issues = unique_index(matrix_rows, "toolchain_id", "PLATFORM_TOOLCHAIN_MATRIX")
    issues = list(index_issues)
    matrix_tokens: set[str] = set()
    toolchain_results: dict[str, Any] = {}
    missing_commands: list[str] = []
    for row in matrix_rows:
        toolchain_id = row.get("toolchain_id", "").strip()
        matrix_tokens.update(lane_tokens(row.get("lanes", "")))
        commands = commands_for_toolchain(row)
        command_results: dict[str, Any] = {}
        for command in commands:
            present = command_exists(command)
            command_results[command] = {
                "present": present,
                "version": command_version(command) if present else None,
            }
            if not present:
                missing_commands.append(f"{toolchain_id}:{command}")
        toolchain_results[toolchain_id] = {
            "lanes": sorted(lane_tokens(row.get("lanes", ""))),
            "status": status_value(row) or "empty",
            "commands": command_results,
        }
    for row in manifest_rows:
        component = row.get("component_id", "").strip()
        if component == DBEAVER_COMPONENT_ID:
            continue
        if not component_is_covered(row, matrix_tokens):
            issues.append(f"toolchain_matrix:{component}:no_lane_token_for_component")
    if missing_commands and not allow_missing:
        issues.extend(f"toolchain_probe:missing_command:{item}" for item in missing_commands)
    return {
        "command": "driver_toolchain_probe.py",
        "gate_id": "BETA-DTA-GATE-024",
        "status": report_status(issues),
        "summary": {
            "manifest_components": len(manifest_rows),
            "toolchain_rows": len(matrix_rows),
            "lane_tokens": len(matrix_tokens),
            "missing_commands": len(missing_commands),
            "allow_missing": allow_missing,
        },
        "toolchains": toolchain_results,
        "issues": issues,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    add_common_args(parser, Path(__file__))
    parser.add_argument(
        "--allow-missing",
        action="store_true",
        default=os.environ.get("SB_DRIVER_TOOLCHAIN_ALLOW_MISSING") == "1",
    )
    args = parser.parse_args()
    repo_root = resolve_repo_root(args.repo_root)
    workplan_root = resolve_workplan_root(repo_root, args.workplan_root)
    output = args.output or default_report_path(repo_root, REPORT_NAME)
    try:
        report = build_report(repo_root, workplan_root, args.allow_missing)
    except (OSError, ValueError) as exc:
        return fail(str(exc))
    write_report(output, report)
    print(f"driver_toolchain_probe={report['status']}")
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
