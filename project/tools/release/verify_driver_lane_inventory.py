#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Verify beta driver/adaptor/tool lane inventory against the package manifest."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Any

from driver_release_common import (
    DBEAVER_COMPONENT_ID,
    DBEAVER_NAME,
    REQUIRED_MANIFEST_FIELDS,
    add_common_args,
    component_label,
    default_report_path,
    fail,
    in_scope_manifest_rows,
    load_manifest,
    load_workplan_csv,
    reject_recorded_private_path,
    report_status,
    resolve_repo_root,
    resolve_workplan_root,
    unique_index,
    write_report,
)


REPORT_NAME = "driver_lane_inventory.json"


def check_manifest_rows(repo_root: Path, rows: list[dict[str, str]]) -> list[str]:
    issues: list[str] = []
    seen_uuids: set[str] = set()
    for row in rows:
        component = component_label(row)
        for field in REQUIRED_MANIFEST_FIELDS:
            if not row.get(field, "").strip():
                issues.append(f"manifest:{component}:missing_{field}")
        expected_component = f"{row.get('category', '').strip()}:{row.get('name', '').strip()}"
        if row.get("component_id", "").strip() != expected_component:
            issues.append(f"manifest:{component}:component_id_mismatch:{expected_component}")
        if row.get("category", "").strip() not in {"driver", "adaptor", "tool"}:
            issues.append(f"manifest:{component}:unknown_category:{row.get('category', '')}")
        package_uuid = row.get("driver_package_uuid", "").strip()
        if package_uuid in seen_uuids:
            issues.append(f"manifest:{component}:duplicate_driver_package_uuid:{package_uuid}")
        seen_uuids.add(package_uuid)
        source_path = row.get("source_path", "").strip()
        issues.extend(reject_recorded_private_path(source_path, f"manifest:{component}:source_path"))
        if source_path and not (repo_root / source_path).is_dir():
            issues.append(f"manifest:{component}:source_path_missing:{source_path}")
    return issues


def check_lane_rows(
    manifest_rows: list[dict[str, str]],
    lane_rows: list[dict[str, str]],
) -> list[str]:
    issues: list[str] = []
    manifest_by_id, manifest_index_issues = unique_index(
        manifest_rows, "component_id", "DriverPackageManifest"
    )
    lane_by_id, lane_index_issues = unique_index(
        lane_rows, "component_id", "LANE_COMPLETION_MATRIX"
    )
    issues.extend(manifest_index_issues)
    issues.extend(lane_index_issues)

    missing_lane_rows = sorted(set(manifest_by_id) - set(lane_by_id))
    extra_lane_rows = sorted(set(lane_by_id) - set(manifest_by_id))
    for component in missing_lane_rows:
        issues.append(f"lane_matrix:missing_component:{component}")
    for component in extra_lane_rows:
        issues.append(f"lane_matrix:extra_component:{component}")

    for component, manifest_row in sorted(manifest_by_id.items()):
        lane_row = lane_by_id.get(component)
        if lane_row is None:
            continue
        for field in ("category", "name", "source_path"):
            if lane_row.get(field, "").strip() != manifest_row.get(field, "").strip():
                issues.append(
                    f"lane_matrix:{component}:{field}_mismatch:"
                    f"{lane_row.get(field, '').strip()}!="
                    f"{manifest_row.get(field, '').strip()}"
                )
        if component == DBEAVER_COMPONENT_ID:
            if not lane_row.get("release_scope", "").startswith("separate_controller"):
                issues.append("lane_matrix:dbeaver:not_marked_separate_controller")
            if lane_row.get("completion_policy", "").strip() != "not_part_of_this_beta_controller":
                issues.append("lane_matrix:dbeaver:completion_policy_not_excluded")
            if lane_row.get("status", "").strip() != "separate_controller":
                issues.append("lane_matrix:dbeaver:status_not_separate_controller")
            continue
        if lane_row.get("release_scope", "").strip() != "in_scope_required":
            issues.append(f"lane_matrix:{component}:release_scope_not_in_scope_required")
        if lane_row.get("completion_policy", "").strip() != "no_deferral_full_implementation_required":
            issues.append(f"lane_matrix:{component}:completion_policy_not_no_deferral")
        if not lane_row.get("required_shared_gates", "").strip():
            issues.append(f"lane_matrix:{component}:required_shared_gates_empty")
    return issues


def build_report(repo_root: Path, workplan_root: Path) -> dict[str, Any]:
    manifest_rows = load_manifest(repo_root)
    lane_rows = load_workplan_csv(workplan_root, "LANE_COMPLETION_MATRIX.csv")
    issues = check_manifest_rows(repo_root, manifest_rows)
    issues.extend(check_lane_rows(manifest_rows, lane_rows))
    in_scope_rows = in_scope_manifest_rows(manifest_rows)
    dbeaver_rows = [
        row for row in manifest_rows if row.get("component_id", "").strip() == DBEAVER_COMPONENT_ID
    ]
    if len(dbeaver_rows) != 1:
        issues.append(f"manifest:{DBEAVER_NAME}:expected_one_explicit_exclusion")
    return {
        "command": "verify_driver_lane_inventory.py",
        "gate_id": "BETA-DTA-GATE-001",
        "status": report_status(issues),
        "summary": {
            "manifest_rows": len(manifest_rows),
            "lane_rows": len(lane_rows),
            "in_scope_components": len(in_scope_rows),
            "explicit_exclusions": len(dbeaver_rows),
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
    print(f"driver_lane_inventory={report['status']}")
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
