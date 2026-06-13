#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Fail release while compatibility variances remain open."""

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


REPORT_NAME = "driver_compatibility_variance.json"
KNOWN_AGGREGATE_SURFACES = {
    "all_lanes",
    "adbc_flightsql",
    "bi_adapters",
    "orm_adapters",
    "language_resources",
    "sblr_bundle_policy",
    "caching_policy",
}


def build_known_lane_tokens(manifest_rows: list[dict[str, str]]) -> set[str]:
    tokens = set(KNOWN_AGGREGATE_SURFACES)
    for row in manifest_rows:
        if row.get("component_id", "").strip() == DBEAVER_COMPONENT_ID:
            continue
        tokens.add(row.get("name", "").strip())
        tokens.add(row.get("driver_family", "").strip())
    return {token for token in tokens if token}


def build_report(repo_root: Path, workplan_root: Path) -> dict[str, Any]:
    manifest_rows = load_manifest(repo_root)
    rows = load_workplan_csv(workplan_root, "COMPATIBILITY_VARIANCE_REGISTER.csv")
    known_tokens = build_known_lane_tokens(manifest_rows)
    by_id, index_issues = unique_index(rows, "variance_id", "COMPATIBILITY_VARIANCE_REGISTER")
    issues = list(index_issues)
    for variance_id, row in sorted(by_id.items()):
        lane = row.get("lane_or_surface", "").strip()
        if lane not in known_tokens:
            issues.append(f"variance_register:{variance_id}:unknown_lane_or_surface:{lane}")
        if row.get("decision_class", "").strip() not in {
            "implement_or_exact_refuse",
            "release_blocking_until_closed",
        }:
            issues.append(f"variance_register:{variance_id}:unknown_decision_class")
        for field in ("variance_description", "required_proof", "release_rule"):
            if not row.get(field, "").strip():
                issues.append(f"variance_register:{variance_id}:missing_{field}")
        if not is_closing_status(status_value(row)):
            issues.append(
                f"variance_register:{variance_id}:non_closing_status:"
                f"{status_value(row) or 'empty'}"
            )
    return {
        "command": "driver_compatibility_variance_gate.py",
        "gate_id": "BETA-DTA-GATE-027",
        "status": report_status(issues),
        "summary": {
            "variance_rows": len(rows),
            "known_lane_tokens": len(known_tokens),
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
    print(f"driver_compatibility_variance={report['status']}")
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
