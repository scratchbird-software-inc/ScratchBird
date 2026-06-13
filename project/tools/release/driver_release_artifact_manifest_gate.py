#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate driver release artifact manifest rows and produced metadata."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

from driver_release_common import (
    DBEAVER_COMPONENT_ID,
    add_common_args,
    dbeaver_output_hits,
    default_report_path,
    fail,
    in_scope_manifest_rows,
    is_closing_status,
    load_manifest,
    load_workplan_csv,
    map_expected_output,
    report_status,
    resolve_repo_root,
    resolve_workplan_root,
    status_value,
    unique_index,
    write_report,
)


REPORT_NAME = "driver_release_artifact_manifest.json"

METADATA_SYNONYMS = {
    "hash": ("hash", "sha256", "digest"),
    "sbom": ("sbom",),
    "license": ("license", "notice"),
    "version": ("version",),
    "source_commit": ("source_commit", "commit", "revision"),
}


def artifact_metadata_tokens(path: Path) -> set[str]:
    tokens: set[str] = set()
    if not path.exists():
        return tokens
    if path.is_file():
        candidates = [path]
    else:
        candidates = [item for item in path.rglob("*") if item.is_file()]
    for item in candidates:
        name = item.name.lower()
        for token, aliases in METADATA_SYNONYMS.items():
            if any(alias in name for alias in aliases):
                tokens.add(token)
    for manifest_name in ("artifact_manifest.json", "package_manifest.json", "manifest.json"):
        manifest = path / manifest_name
        if not manifest.is_file():
            continue
        try:
            data = json.loads(manifest.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        keys = {str(key).lower() for key in data.keys()} if isinstance(data, dict) else set()
        for token, aliases in METADATA_SYNONYMS.items():
            if token in keys or any(alias in keys for alias in aliases):
                tokens.add(token)
    return tokens


def required_metadata_tokens(value: str) -> set[str]:
    required: set[str] = set()
    text = value.lower()
    for token, aliases in METADATA_SYNONYMS.items():
        if token in text or any(alias in text for alias in aliases):
            required.add(token)
    return required


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
    for component in sorted(expected_components):
        row = artifact_by_component.get(component)
        if row is None:
            issues.append(f"artifact_manifest:{component}:missing_row")
            continue
        if row.get("release_scope", "").strip() != "in_scope_required":
            issues.append(f"artifact_manifest:{component}:release_scope_not_in_scope_required")
        if not is_closing_status(status_value(row)):
            issues.append(
                f"artifact_manifest:{component}:non_closing_status:{status_value(row) or 'empty'}"
            )
        artifact_path = map_expected_output(row.get("expected_public_output", ""), output_root, repo_root)
        if artifact_path is None:
            issues.append(f"artifact_manifest:{component}:expected_output_none")
            continue
        if not artifact_path.exists():
            issues.append(
                f"artifact_manifest:{component}:expected_output_missing:"
                f"{row.get('expected_public_output', '')}"
            )
            continue
        present = artifact_metadata_tokens(artifact_path)
        required = required_metadata_tokens(row.get("required_metadata", ""))
        for token in sorted(required - present):
            issues.append(f"artifact_manifest:{component}:metadata_missing:{token}")

    for component in sorted(set(artifact_by_component) - set(manifest_by_id)):
        issues.append(f"artifact_manifest:{component}:not_in_manifest")

    dbeaver_row = artifact_by_component.get(DBEAVER_COMPONENT_ID)
    if dbeaver_row is None:
        issues.append("artifact_manifest:dbeaver_exclusion_row_missing")
    else:
        if dbeaver_row.get("status", "").strip() != "excluded":
            issues.append("artifact_manifest:dbeaver_status_not_excluded")
        if dbeaver_row.get("expected_public_output", "").strip().lower() != "none":
            issues.append("artifact_manifest:dbeaver_expected_output_not_none")
    for hit in dbeaver_output_hits(output_root):
        issues.append(f"artifact_manifest:dbeaver_output_present:{hit}")

    return {
        "command": "driver_release_artifact_manifest_gate.py",
        "gate_id": "BETA-DTA-GATE-028",
        "status": report_status(issues),
        "summary": {
            "manifest_components": len(manifest_by_id),
            "in_scope_components": len(expected_components),
            "artifact_rows": len(artifact_rows),
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
    output_root = args.output_root if args.output_root.is_absolute() else repo_root / args.output_root
    output = args.output or default_report_path(repo_root, REPORT_NAME)
    try:
        report = build_report(repo_root, workplan_root, output_root.resolve())
    except (OSError, ValueError) as exc:
        return fail(str(exc))
    write_report(output, report)
    print(f"driver_release_artifact_manifest={report['status']}")
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
