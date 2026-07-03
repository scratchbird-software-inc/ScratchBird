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


def driver_name_from_component(component_id: str) -> str:
    if component_id.startswith("driver:"):
        return component_id.split(":", 1)[1]
    return component_id


def read_report(path: Path) -> dict[str, Any] | None:
    if not path.is_file():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return {"status": "fail", "issues": [f"invalid_json:{path}"]}


def retained_full_surface_proof_status(
    workplan_root: Path,
    matrix_rows: list[dict[str, str]],
) -> tuple[bool, list[str], dict[str, str]]:
    proof_root = workplan_root / "artifacts" / "driver-full-surface"
    detailed_lane_artifacts = (
        "HASHES.txt",
        "command-events.jsonl",
        "junit.xml",
        "native-api-coverage.json",
        "route-environment.json",
        "security-refusals.json",
        "summary.json",
        "wire-transcript.jsonl",
    )
    issues: list[str] = []
    statuses: dict[str, str] = {}
    for row in matrix_rows:
        component = row.get("component_id", "").strip()
        driver = driver_name_from_component(component)
        lane_root = proof_root / f"{driver}_tls_noncluster_profile8"
        summary_path = lane_root / "summary.json"
        matrix_path = lane_root / f"driver_native_full_surface_matrix_{driver}_tls_noncluster_profile8.json"
        artifact_gate_path = (
            lane_root
            / f"driver_native_full_surface_matrix_{driver}_tls_noncluster_profile8_artifact_gate.json"
        )
        if not lane_root.is_dir():
            issues.append(f"retained_full_surface_proof:{component}:missing_directory")
            statuses[component] = "missing"
            continue
        missing_artifacts = [name for name in detailed_lane_artifacts if not (lane_root / name).is_file()]
        has_matrix_wrapper = matrix_path.is_file() and artifact_gate_path.is_file()
        if missing_artifacts and not has_matrix_wrapper:
            issues.append(
                f"retained_full_surface_proof:{component}:missing_required_artifacts:{';'.join(missing_artifacts)}"
            )
            statuses[component] = "missing_required_artifacts"
            continue
        try:
            summary = json.loads(summary_path.read_text(encoding="utf-8"))
        except FileNotFoundError as exc:
            issues.append(f"retained_full_surface_proof:{component}:missing_file:{Path(exc.filename).name}")
            statuses[component] = "missing_file"
            continue
        except json.JSONDecodeError as exc:
            issues.append(f"retained_full_surface_proof:{component}:invalid_json:{exc.doc[:40]}")
            statuses[component] = "invalid_json"
            continue
        if summary.get("status") != "pass":
            issues.append(
                f"retained_full_surface_proof:{component}:summary_status:{summary.get('status') or 'empty'}"
            )
        if summary.get("failure_count") not in (0, "0", None):
            issues.append(
                f"retained_full_surface_proof:{component}:summary_failure_count:{summary.get('failure_count')}"
            )
        full_surface_proven = (
            summary.get("full_surface_language_corpus_executed") is True
            or int(summary.get("statement_count") or 0) >= 8554
        )
        if not full_surface_proven:
            issues.append(f"retained_full_surface_proof:{component}:full_surface_corpus_not_executed")
        if summary.get("server_revalidation_required") is not True:
            issues.append(f"retained_full_surface_proof:{component}:server_revalidation_not_proven")
        if matrix_path.is_file():
            try:
                matrix = json.loads(matrix_path.read_text(encoding="utf-8"))
            except json.JSONDecodeError as exc:
                issues.append(f"retained_full_surface_proof:{component}:invalid_matrix_json:{exc.doc[:40]}")
            else:
                if matrix.get("status") != "pass":
                    issues.append(
                        f"retained_full_surface_proof:{component}:matrix_status:{matrix.get('status') or 'empty'}"
                    )
                if matrix.get("failure_count") != 0:
                    issues.append(
                        f"retained_full_surface_proof:{component}:matrix_failure_count:{matrix.get('failure_count')}"
                    )
        if artifact_gate_path.is_file():
            try:
                artifact_gate = json.loads(artifact_gate_path.read_text(encoding="utf-8"))
            except json.JSONDecodeError as exc:
                issues.append(
                    f"retained_full_surface_proof:{component}:invalid_artifact_gate_json:{exc.doc[:40]}"
                )
            else:
                if artifact_gate.get("status") != "pass":
                    issues.append(
                        f"retained_full_surface_proof:{component}:artifact_gate_status:{artifact_gate.get('status') or 'empty'}"
                    )
        statuses[component] = "pass" if not any(component in issue for issue in issues) else "fail"
    return not issues, issues, statuses


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
        if name == "driver_complete_coverage_tests.json" and status == "preflight_pass":
            retained_ok, retained_issues, retained_statuses = retained_full_surface_proof_status(
                workplan_root,
                matrix_rows,
            )
            report_statuses["retained_full_surface_profile8"] = "pass" if retained_ok else "fail"
            report_statuses["retained_full_surface_profile8_by_driver"] = json.dumps(
                retained_statuses,
                sort_keys=True,
            )
            issues.extend(retained_issues)
            if retained_ok:
                continue
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
