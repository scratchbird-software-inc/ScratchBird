#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Verify beta driver release output inclusion and DBeaver exclusion."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Any

from driver_release_common import (
    DBEAVER_COMPONENT_ID,
    add_common_args,
    dbeaver_output_hits,
    default_report_path,
    fail,
    in_scope_manifest_rows,
    load_manifest,
    load_workplan_csv,
    map_expected_output,
    report_status,
    resolve_repo_root,
    resolve_workplan_root,
    unique_index,
    write_report,
)


REPORT_NAME = "driver_beta_release_verify.json"


def build_report(repo_root: Path, workplan_root: Path, output_root: Path) -> dict[str, Any]:
    manifest_rows = load_manifest(repo_root)
    artifact_rows = load_workplan_csv(workplan_root, "RELEASE_ARTIFACT_MANIFEST_MATRIX.csv")
    manifest_by_id, manifest_issues = unique_index(
        manifest_rows, "component_id", "DriverPackageManifest"
    )
    artifact_by_component, artifact_issues = unique_index(
        artifact_rows, "component_id", "RELEASE_ARTIFACT_MANIFEST_MATRIX"
    )
    issues = list(manifest_issues) + list(artifact_issues)

    expected_components = {row["component_id"] for row in in_scope_manifest_rows(manifest_rows)}
    artifact_components = {
        component
        for component, row in artifact_by_component.items()
        if row.get("release_scope", "").strip() == "in_scope_required"
    }
    for component in sorted(expected_components - artifact_components):
        issues.append(f"release_output:missing_artifact_row:{component}")
    for component in sorted(artifact_components - expected_components):
        issues.append(f"release_output:unexpected_artifact_row:{component}")

    missing_outputs: list[str] = []
    for component in sorted(expected_components):
        row = artifact_by_component.get(component)
        if row is None:
            continue
        expected = row.get("expected_public_output", "")
        output = map_expected_output(expected, output_root, repo_root)
        if output is None:
            issues.append(f"release_output:{component}:expected_output_none")
            continue
        if not output.exists():
            missing_outputs.append(f"{component}:{expected}")
    issues.extend(f"release_output:missing_expected_output:{item}" for item in missing_outputs)

    dbeaver_row = artifact_by_component.get(DBEAVER_COMPONENT_ID)
    if dbeaver_row is None:
        issues.append("release_output:dbeaver_exclusion_row_missing")
    else:
        if dbeaver_row.get("release_scope", "").strip() != "separate_controller":
            issues.append("release_output:dbeaver_release_scope_not_separate_controller")
        if dbeaver_row.get("expected_public_output", "").strip().lower() != "none":
            issues.append("release_output:dbeaver_expected_output_not_none")

    for hit in dbeaver_output_hits(output_root):
        issues.append(f"release_output:dbeaver_output_present:{hit}")

    return {
        "command": "verify_driver_beta_release.py",
        "gate_id": "BETA-DTA-GATE-019",
        "status": report_status(issues),
        "summary": {
            "manifest_components": len(manifest_by_id),
            "in_scope_components": len(expected_components),
            "artifact_rows": len(artifact_rows),
            "missing_outputs": len(missing_outputs),
            "output_root_exists": output_root.exists(),
        },
        "issues": issues,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    add_common_args(parser, Path(__file__))
    parser.add_argument("output_root", nargs="?", type=Path, default=Path("build/output"))
    args = parser.parse_args()
    repo_root = resolve_repo_root(args.repo_root)
    workplan_root = resolve_workplan_root(repo_root, args.workplan_root)
    output_root = args.output_root
    if not output_root.is_absolute():
        output_root = repo_root / output_root
    output = args.output or default_report_path(repo_root, REPORT_NAME)
    try:
        report = build_report(repo_root, workplan_root, output_root.resolve())
    except (OSError, ValueError) as exc:
        return fail(str(exc))
    write_report(output, report)
    print(f"driver_beta_release_verify={report['status']}")
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
