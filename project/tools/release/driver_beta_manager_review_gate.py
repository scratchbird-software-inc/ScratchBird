#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Verify manager-review closure inputs for the beta driver release."""

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


REPORT_NAME = "driver_beta_manager_review.json"


def check_status_rows(
    rows: list[dict[str, str]],
    id_field: str,
    context: str,
    required_ids: set[str] | None = None,
) -> list[str]:
    by_id, issues = unique_index(rows, id_field, context)
    if required_ids:
        for row_id in sorted(required_ids - set(by_id)):
            issues.append(f"{context}:missing_required_row:{row_id}")
    for row_id, row in sorted(by_id.items()):
        if required_ids is not None and row_id not in required_ids:
            continue
        if not is_closing_status(status_value(row)):
            issues.append(f"{context}:{row_id}:non_closing_status:{status_value(row) or 'empty'}")
    return issues


def build_report(workplan_root: Path) -> dict[str, Any]:
    audit_rows = load_workplan_csv(workplan_root, "SPEC_IMPLEMENTATION_AUDIT_MATRIX.csv")
    proof_rows = load_workplan_csv(workplan_root, "PROOF_EVIDENCE_LEDGER.csv")
    gate_rows = load_workplan_csv(workplan_root, "ACCEPTANCE_GATES.csv")
    issues = check_status_rows(audit_rows, "audit_id", "SPEC_IMPLEMENTATION_AUDIT_MATRIX")
    issues.extend(
        check_status_rows(
            proof_rows,
            "proof_id",
            "PROOF_EVIDENCE_LEDGER",
            required_ids={"BETA-DTA-PROOF-009"},
        )
    )
    issues.extend(
        check_status_rows(
            gate_rows,
            "gate_id",
            "ACCEPTANCE_GATES",
            required_ids={"BETA-DTA-GATE-020"},
        )
    )
    return {
        "command": "driver_beta_manager_review_gate.py",
        "gate_id": "BETA-DTA-GATE-020",
        "status": report_status(issues),
        "summary": {
            "audit_rows": len(audit_rows),
            "proof_rows": len(proof_rows),
            "acceptance_gate_rows": len(gate_rows),
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
        report = build_report(workplan_root)
    except (OSError, ValueError) as exc:
        return fail(str(exc))
    write_report(output, report)
    print(f"driver_beta_manager_review={report['status']}")
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
