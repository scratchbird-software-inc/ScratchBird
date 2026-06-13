#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Read-only final go/no-go gate for beta driver release closure."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Any

from driver_release_common import (
    add_common_args,
    default_report_path,
    fail,
    is_closing_status,
    load_workplan_csv,
    report_status,
    resolve_repo_root,
    resolve_workplan_root,
    status_value,
    unique_index,
    write_report,
)


REPORT_NAME = "driver_beta_go_no_go.json"
STATUS_FILES = (
    ("TRACKER.csv", "slice_id"),
    ("ACCEPTANCE_GATES.csv", "gate_id"),
    ("LANE_COMPLETION_MATRIX.csv", "component_id"),
    ("AGENT_LANE_INDEX.csv", "agent_row_id"),
    ("IMPLEMENTATION_SEQUENCE_DAG.csv", "node_id"),
    ("GATE_COMMAND_REGISTRY.csv", "command_id"),
    ("PLATFORM_TOOLCHAIN_MATRIX.csv", "toolchain_id"),
    ("LIVE_SERVER_TEST_FIXTURE_MATRIX.csv", "fixture_id"),
    ("WIRE_TRANSCRIPT_ORACLE_MATRIX.csv", "oracle_id"),
    ("COMPATIBILITY_VARIANCE_REGISTER.csv", "variance_id"),
    ("RELEASE_ARTIFACT_MANIFEST_MATRIX.csv", "component_id"),
    ("RESOURCE_LIMIT_AND_SOAK_MATRIX.csv", "resource_gate_id"),
    ("SHARED_CONFORMANCE_SUITE_MATRIX.csv", "suite_id"),
    ("DRIVER_CAPABILITY_BASELINE_MATRIX.csv", "capability_id"),
    ("SERVER_AUTHORIZATION_AND_CACHE_MATRIX.csv", "control_id"),
    ("LANGUAGE_RESOURCE_DRIVER_MATRIX.csv", "resource_id"),
    ("SPEC_IMPLEMENTATION_AUDIT_MATRIX.csv", "audit_id"),
    ("PROOF_EVIDENCE_LEDGER.csv", "proof_id"),
)


def read_go_no_go(path: Path, issues: list[str]) -> str:
    try:
        text = path.read_text(encoding="utf-8")
    except FileNotFoundError:
        issues.append("GO_NO_GO:missing_file")
        return "missing"
    if "BETA-DTA-GO-NO-GO" not in text:
        issues.append("GO_NO_GO:missing_search_key")
    for line in text.splitlines():
        if line.strip().lower().startswith("current decision:"):
            return line.split(":", 1)[1].strip().strip("`").strip(".").lower()
    issues.append("GO_NO_GO:missing_current_decision")
    return "missing"


def build_report(workplan_root: Path) -> dict[str, Any]:
    issues: list[str] = []
    incomplete: dict[str, list[str]] = {}
    decision = read_go_no_go(workplan_root / "GO_NO_GO.md", issues)
    if decision != "go":
        issues.append(f"GO_NO_GO:decision_not_go:{decision}")

    for filename, id_field in STATUS_FILES:
        rows = load_workplan_csv(workplan_root, filename)
        by_id, file_issues = unique_index(rows, id_field, filename)
        issues.extend(file_issues)
        file_incomplete: list[str] = []
        for row_id, row in sorted(by_id.items()):
            if not is_closing_status(status_value(row)):
                file_incomplete.append(f"{row_id}:{status_value(row) or 'empty'}")
        if file_incomplete:
            incomplete[filename] = file_incomplete

    for filename, entries in sorted(incomplete.items()):
        issues.append(f"{filename}:non_closing_rows:{len(entries)}")

    return {
        "command": "driver_beta_go_no_go_gate.py",
        "gate_id": "BETA-DTA-GATE-031",
        "status": report_status(issues),
        "summary": {
            "decision": decision,
            "status_files": len(STATUS_FILES),
            "files_with_incomplete_rows": len(incomplete),
            "incomplete_rows": sum(len(entries) for entries in incomplete.values()),
        },
        "issues": issues,
        "incomplete": incomplete,
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
    print(f"driver_beta_go_no_go={report['status']}")
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
