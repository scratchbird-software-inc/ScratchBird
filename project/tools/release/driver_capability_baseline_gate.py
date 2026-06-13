#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate beta driver baseline capabilities and server authority controls."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Any

from driver_release_common import (
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


REPORT_NAME = "driver_capability_baseline.json"

REQUIRED_CAPABILITY_GROUPS = {
    "connection_lifecycle",
    "authentication_tls",
    "statement_prepare_execute",
    "parameter_binding",
    "result_fetch",
    "metadata_catalog",
    "diagnostics",
    "transactions",
    "cancellation_timeout",
    "pooling_threading",
    "sbsql_intelligence",
    "sblr_uuid_bundle",
    "schema_path_resolution",
    "caching",
    "bulk_batch_streaming",
    "package_distribution",
    "best_in_class_delta",
    "exact_refusal",
}

SERVER_AUTHORITY_AREAS = {
    "sblr_admission",
    "uuid_authority",
    "transaction_context",
    "policy_epoch",
    "schema_epoch",
    "language_epoch",
    "result_cache",
    "capability_claims",
    "reverse_path_resolution",
    "message_redaction",
}


def check_capability_rows(rows: list[dict[str, str]]) -> list[str]:
    issues: list[str] = []
    by_id, index_issues = unique_index(rows, "capability_id", "DRIVER_CAPABILITY_BASELINE_MATRIX")
    issues.extend(index_issues)
    groups = {row.get("capability_group", "").strip() for row in by_id.values()}
    for group in sorted(REQUIRED_CAPABILITY_GROUPS - groups):
        issues.append(f"capability_matrix:missing_group:{group}")

    for capability_id, row in sorted(by_id.items()):
        group = row.get("capability_group", "").strip()
        for field in ("baseline_requirement", "host_api_mapping", "proof"):
            if not row.get(field, "").strip():
                issues.append(f"capability_matrix:{capability_id}:missing_{field}")
        if not is_closing_status(status_value(row)):
            issues.append(
                f"capability_matrix:{capability_id}:non_closing_status:"
                f"{status_value(row) or 'empty'}"
            )
        if group == "transactions":
            combined = (
                row.get("baseline_requirement", "")
                + " "
                + row.get("proof", "")
                + " "
                + row.get("host_api_mapping", "")
            ).lower()
            for token in ("autocommit", "commit", "rollback", "savepoint", "mga"):
                if token not in combined:
                    issues.append(f"capability_matrix:{capability_id}:missing_transaction_token:{token}")
        if group in {"sblr_uuid_bundle", "caching"}:
            combined = (row.get("baseline_requirement", "") + " " + row.get("proof", "")).lower()
            if "server" not in combined and "revalidation" not in combined:
                issues.append(f"capability_matrix:{capability_id}:missing_server_revalidation_reference")
    return issues


def check_server_authority(rows: list[dict[str, str]]) -> list[str]:
    issues: list[str] = []
    by_id, index_issues = unique_index(rows, "control_id", "SERVER_AUTHORIZATION_AND_CACHE_MATRIX")
    issues.extend(index_issues)
    areas = {row.get("area", "").strip() for row in by_id.values()}
    for area in sorted(SERVER_AUTHORITY_AREAS - areas):
        issues.append(f"server_authority_matrix:missing_area:{area}")
    for control_id, row in sorted(by_id.items()):
        area = row.get("area", "").strip()
        for field in ("server_requirement", "driver_requirement", "proof"):
            if not row.get(field, "").strip():
                issues.append(f"server_authority_matrix:{control_id}:missing_{field}")
        if area in SERVER_AUTHORITY_AREAS and not is_closing_status(status_value(row)):
            issues.append(
                f"server_authority_matrix:{control_id}:non_closing_status:"
                f"{status_value(row) or 'empty'}"
            )
        if area in {"sblr_admission", "uuid_authority", "result_cache", "capability_claims"}:
            if "Server" not in row.get("server_requirement", ""):
                issues.append(f"server_authority_matrix:{control_id}:server_requirement_not_server_owned")
    return issues


def build_report(repo_root: Path, workplan_root: Path) -> dict[str, Any]:
    manifest_rows = load_manifest(repo_root)
    capability_rows = load_workplan_csv(workplan_root, "DRIVER_CAPABILITY_BASELINE_MATRIX.csv")
    authority_rows = load_workplan_csv(workplan_root, "SERVER_AUTHORIZATION_AND_CACHE_MATRIX.csv")
    issues = check_capability_rows(capability_rows)
    issues.extend(check_server_authority(authority_rows))
    return {
        "command": "driver_capability_baseline_gate.py",
        "gate_id": "BETA-DTA-GATE-003",
        "status": report_status(issues),
        "summary": {
            "manifest_components": len(manifest_rows),
            "capability_rows": len(capability_rows),
            "server_authority_rows": len(authority_rows),
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
    print(f"driver_capability_baseline={report['status']}")
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
