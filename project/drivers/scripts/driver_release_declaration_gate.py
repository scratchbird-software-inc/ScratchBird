#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate or validate driver/server reconciliation release evidence.

Validation is intentionally read-only. Generation summarizes existing evidence
and must not promote checklist or evidence rows to passing states.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import sys
from pathlib import Path
from typing import Any

import yaml


DECLARATION_JSON = "DRIVER_SERVER_RELEASE_DECLARATION.json"
DECLARATION_CSV = "DRIVER_SERVER_RELEASE_DECLARATION.csv"
REQUIRED_P4_ARTIFACTS = (
    "SERVER_VERIFICATION_PACKETS.json",
    "FULL_ROUTE_BENCHMARK_EVIDENCE.json",
    "PERFORMANCE_BUDGETS.json",
    "DSR_044_DOCUMENTATION_SAMPLE_APP_EVIDENCE.json",
    "DSR_045_REFERENCE_DRIVER_COMPATIBILITY_ROUTE_EVIDENCE.json",
)
REQUIRED_CLOSED_GATES = {
    "DSR_G00",
    "DSR_G01",
    "DSR_G02",
    "DSR_G02A",
    "DSR_G03",
    "DSR_G04",
    "DSR_G05",
    "DSR_G06",
    "DSR_G07",
    "DSR_G08",
    "DSR_G09",
    "DSR_G10",
    "DSR_G11",
    "DSR_G14",
    "DSR_G15",
    "DSR_G16",
    "DSR_G17",
    "DSR_G18",
    "DSR_G19",
    "DSR_G20",
}
ALLOWED_RELEASE_BUCKETS = {
    "release_supported",
    "release_candidate",
    "tracked_not_released",
}


def fail(message: str) -> int:
    print(f"failed: {message}", file=sys.stderr)
    return 1


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def write_csv(path: Path, rows: list[dict[str, str]], fieldnames: list[str]) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def load_yaml(path: Path) -> dict[str, Any]:
    with path.open(encoding="utf-8") as handle:
        data = yaml.safe_load(handle)
    if not isinstance(data, dict):
        raise ValueError(f"{path} must contain a YAML mapping")
    return data


def trace_manifest_name(category: str, name: str) -> str:
    if category == "driver":
        return f"driver-{name}-checklist-status.yaml"
    if category == "adaptor":
        return f"adapter-{name}-checklist-status.yaml"
    if category == "tool":
        return f"tool-{name}-checklist-status.yaml"
    raise ValueError(f"unknown component category {category!r}")


def artifact_root(execution_plan_root: Path) -> Path:
    return execution_plan_root / "artifacts"


def load_target_rows(execution_plan_root: Path) -> list[dict[str, str]]:
    path = artifact_root(execution_plan_root) / "TARGET_CHECKLIST_ROWS.csv"
    return read_csv(path)


def load_target_evidence(execution_plan_root: Path) -> list[dict[str, str]]:
    path = artifact_root(execution_plan_root) / "TARGET_EVIDENCE_MANIFEST.csv"
    return read_csv(path)


def load_component_lanes(repo_root: Path, project_root: Path) -> list[dict[str, Any]]:
    manifest_rows = read_csv(project_root / "drivers" / "DriverPackageManifest.csv")
    trace_root = repo_root / "docs" / "contracts" / "trace"
    lanes: list[dict[str, Any]] = []
    for component in manifest_rows:
        component_id = component["component_id"]
        name = component["name"]
        category = component["category"]
        release_bucket = component.get("release_bucket", "").strip()
        if not release_bucket:
            raise ValueError(f"{component_id} is missing release_bucket")
        if release_bucket not in ALLOWED_RELEASE_BUCKETS:
            raise ValueError(
                f"{component_id} has invalid release_bucket {release_bucket!r}"
            )
        trace_path = trace_root / trace_manifest_name(category, name)
        if trace_path.is_file():
            trace = load_yaml(trace_path)
            items = trace.get("items", [])
            if not isinstance(items, list):
                raise ValueError(f"{trace_path} items must be a list")
            trace_manifest = str(trace_path.relative_to(repo_root))
        elif release_bucket == "release_supported":
            raise ValueError(f"{component_id} is release_supported but missing trace manifest")
        else:
            items = []
            trace_manifest = ""
        status_counts: dict[str, int] = {}
        for item in items:
            if not isinstance(item, dict):
                raise ValueError(f"{trace_path} contains a non-mapping item")
            status = str(item.get("status", ""))
            status_counts[status] = status_counts.get(status, 0) + 1
        if not status_counts:
            status_counts["not_required_for_release_candidate"] = 1
        lanes.append(
            {
                "component_id": component_id,
                "category": category,
                "name": name,
                "driver_status": component.get("driver_status", ""),
                "conformance_ctest_label": component.get("conformance_profile_ref", ""),
                "trace_manifest": trace_manifest,
                "package_manifest_ref": f"project/drivers/DriverPackageManifest.csv#component_id={component_id}",
                "row_count": len(items),
                "status_counts": status_counts,
                "release_state": release_bucket,
            }
        )
    return lanes


def release_state_counts(rows: list[dict[str, Any]]) -> dict[str, int]:
    counts: dict[str, int] = {}
    for row in rows:
        state = str(row.get("release_state", ""))
        counts[state] = counts.get(state, 0) + 1
    return counts


def build_declaration(
    repo_root: Path,
    project_root: Path,
    execution_plan_root: Path,
    target_rows: list[dict[str, str]],
    target_evidence: list[dict[str, str]],
) -> dict[str, Any]:
    lanes = load_component_lanes(repo_root, project_root)
    gates = read_csv(execution_plan_root / "ACCEPTANCE_GATES.csv")
    implementation_ahead = read_csv(artifact_root(execution_plan_root) / "IMPLEMENTATION_AHEAD_CLASSIFICATION.csv")
    rows = []
    evidence_by_id = {row["id"]: row for row in target_evidence}
    for row in target_rows:
        evidence = evidence_by_id.get(row["id"], {})
        rows.append(
            {
                "id": row["id"],
                "section": row["section"],
                "capability": row["capability"],
                "requirement": row["requirement"],
                "applies_to": row["applies_to"],
                "release_state": "release_candidate",
                "spec_status": row["spec_status"],
                "implementation_status": row["implementation_status"],
                "test_status": row["test_status"],
                "closure_status": row["closure_status"],
                "required_ctest_labels": evidence.get("required_ctest_labels", ""),
                "route_requirement": evidence.get("route_requirement", ""),
            }
        )
    row_release_state_counts = release_state_counts(rows)
    lane_release_state_counts = release_state_counts(lanes)
    return {
        "schema": "scratchbird.driver_server_release_declaration.v1",
        "generated_at_utc": dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
        "execution_plan": str(execution_plan_root.relative_to(repo_root)),
        "summary": {
            "target_checklist_rows": len(target_rows),
            "target_evidence_rows": len(target_evidence),
            "lanes": len(lanes),
            "implementation_ahead_items": len(implementation_ahead),
            "release_state_counts": row_release_state_counts,
            "lane_release_state_counts": lane_release_state_counts,
        },
        "required_artifacts": [str((artifact_root(execution_plan_root) / name).relative_to(repo_root)) for name in REQUIRED_P4_ARTIFACTS],
        "acceptance_gates": gates,
        "implementation_ahead": implementation_ahead,
        "lanes": lanes,
        "rows": rows,
    }


def write_declaration(repo_root: Path, execution_plan_root: Path, declaration: dict[str, Any]) -> None:
    artifacts = artifact_root(execution_plan_root)
    json_path = artifacts / DECLARATION_JSON
    csv_path = artifacts / DECLARATION_CSV
    json_path.write_text(json.dumps(declaration, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    row_fieldnames = [
        "id",
        "section",
        "capability",
        "requirement",
        "applies_to",
        "release_state",
        "spec_status",
        "implementation_status",
        "test_status",
        "closure_status",
        "required_ctest_labels",
        "route_requirement",
    ]
    with csv_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=row_fieldnames, lineterminator="\n")
        writer.writeheader()
        writer.writerows(declaration["rows"])


def validate(repo_root: Path, project_root: Path, execution_plan_root: Path) -> list[str]:
    errors: list[str] = []
    artifacts = artifact_root(execution_plan_root)
    manifest_lanes = load_component_lanes(repo_root, project_root)
    target_rows = read_csv(artifacts / "TARGET_CHECKLIST_ROWS.csv")
    target_evidence = read_csv(artifacts / "TARGET_EVIDENCE_MANIFEST.csv")
    if len(target_rows) != len(target_evidence):
        errors.append(f"target row/evidence length mismatch: {len(target_rows)} != {len(target_evidence)}")
    for row in target_rows:
        if row.get("spec_status") != "specified":
            errors.append(f"{row.get('id')} spec_status is not specified")
        if row.get("implementation_status") != "implemented_and_proven":
            errors.append(f"{row.get('id')} implementation_status is not implemented_and_proven")
        if row.get("test_status") != "passed":
            errors.append(f"{row.get('id')} test_status is not passed")
        if row.get("closure_status") != "implemented_and_proven":
            errors.append(f"{row.get('id')} closure_status is not implemented_and_proven")
    for row in target_evidence:
        if row.get("status") != "implemented_and_proven":
            errors.append(f"{row.get('id')} evidence status is not implemented_and_proven")

    implementation_ahead = read_csv(artifacts / "IMPLEMENTATION_AHEAD_CLASSIFICATION.csv")
    for row in implementation_ahead:
        if row.get("status") != "completed":
            errors.append(f"{row.get('audit_id')} implementation-ahead status is not completed")

    gates = {row["gate_id"]: row for row in read_csv(execution_plan_root / "ACCEPTANCE_GATES.csv")}
    for gate_id in sorted(REQUIRED_CLOSED_GATES):
        if gates.get(gate_id, {}).get("status") != "completed":
            errors.append(f"{gate_id} is not completed")

    for artifact in REQUIRED_P4_ARTIFACTS:
        if not (artifacts / artifact).is_file():
            errors.append(f"missing release evidence artifact {artifact}")

    declaration_path = artifacts / DECLARATION_JSON
    declaration_csv = artifacts / DECLARATION_CSV
    if not declaration_path.is_file():
        errors.append(f"missing {DECLARATION_JSON}")
    else:
        declaration = json.loads(declaration_path.read_text(encoding="utf-8"))
        if declaration.get("schema") != "scratchbird.driver_server_release_declaration.v1":
            errors.append("release declaration schema mismatch")
        if declaration.get("summary", {}).get("target_checklist_rows") != len(target_rows):
            errors.append("release declaration target row count mismatch")
        if len(declaration.get("rows", [])) != len(target_rows):
            errors.append("release declaration rows do not cover every target checklist row")
        if len(declaration.get("lanes", [])) != len(manifest_lanes):
            errors.append("release declaration lanes do not cover every package manifest component")
        declared_rows = declaration.get("rows", [])
        if isinstance(declared_rows, list):
            declared_counts = release_state_counts(
                [row for row in declared_rows if isinstance(row, dict)]
            )
            if declaration.get("summary", {}).get("release_state_counts") != declared_counts:
                errors.append("release declaration row release-state counts are stale")
            supported_lanes = [
                lane for lane in manifest_lanes if lane.get("release_state") == "release_supported"
            ]
            if not supported_lanes and declared_counts.get("supported"):
                errors.append("release declaration uses legacy supported state without supported lanes")
            if not supported_lanes and declared_counts.get("release_supported"):
                errors.append("release declaration claims release_supported rows without supported lanes")
        declared_lanes = declaration.get("lanes", [])
        if isinstance(declared_lanes, list):
            lane_counts = release_state_counts(
                [lane for lane in declared_lanes if isinstance(lane, dict)]
            )
            if declaration.get("summary", {}).get("lane_release_state_counts") != lane_counts:
                errors.append("release declaration lane release-state counts are stale")
    if not declaration_csv.is_file():
        errors.append(f"missing {DECLARATION_CSV}")
    return errors


def command_generate(args: argparse.Namespace) -> int:
    repo_root = args.repo_root.resolve()
    project_root = args.project_root.resolve()
    execution_plan_root = args.execution_plan_root.resolve()
    try:
        target_rows = load_target_rows(execution_plan_root)
        target_evidence = load_target_evidence(execution_plan_root)
        declaration = build_declaration(repo_root, project_root, execution_plan_root, target_rows, target_evidence)
        write_declaration(repo_root, execution_plan_root, declaration)
    except (OSError, ValueError, KeyError, yaml.YAMLError) as exc:
        return fail(str(exc))
    print(
        "release-declaration generated: "
        f"{len(target_rows)} checklist rows, {declaration['summary']['lanes']} lanes"
    )
    return 0


def command_validate(args: argparse.Namespace) -> int:
    repo_root = args.repo_root.resolve()
    project_root = args.project_root.resolve()
    execution_plan_root = args.execution_plan_root.resolve()
    try:
        errors = validate(repo_root, project_root, execution_plan_root)
    except (OSError, ValueError, KeyError, json.JSONDecodeError, yaml.YAMLError) as exc:
        return fail(str(exc))
    if errors:
        for error in errors[:100]:
            print(f"release-declaration error: {error}", file=sys.stderr)
        if len(errors) > 100:
            print(f"... {len(errors) - 100} additional errors omitted", file=sys.stderr)
        return 1
    print("release-declaration ok")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--fixture-root", "--execution_plan-root", dest="execution_plan_root", type=Path, required=True)
    parser.add_argument("mode", choices=("generate", "validate"))
    args = parser.parse_args()
    if args.mode == "generate":
        return command_generate(args)
    if args.mode == "validate":
        return command_validate(args)
    return fail("unsupported mode")


if __name__ == "__main__":
    raise SystemExit(main())
