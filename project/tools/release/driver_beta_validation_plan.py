#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Verify beta driver validation-plan controller consistency.

This gate is intentionally read-only. It checks that the selected validation
controller references runnable command rows, gate rows, and verification rows
instead of creating or mutating release evidence.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
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


REPORT_NAME = "driver_beta_validation_plan.json"
REQUIRED_FILES = (
    "VALIDATION_PLAN.md",
    "GO_NO_GO.md",
    "GATE_COMMAND_REGISTRY.csv",
    "ACCEPTANCE_GATES.csv",
    "PROOF_EVIDENCE_LEDGER.csv",
    "IMPLEMENTATION_SEQUENCE_DAG.csv",
)
REQUIRED_COMMAND_IDS = {f"BETA-DTA-CMD-{index:03d}" for index in range(1, 32)}
REQUIRED_GATE_IDS = {f"BETA-DTA-GATE-{index:03d}" for index in range(1, 32)}
REQUIRED_PROOF_IDS = {f"BETA-DTA-PROOF-{index:03d}" for index in range(1, 13)}


def ids_in_text(text: str, prefix: str) -> set[str]:
    return set(re.findall(rf"\b{re.escape(prefix)}-\d{{3}}\b", text))


def nonclosing_ids(rows: list[dict[str, str]], id_field: str) -> list[str]:
    return [
        row.get(id_field, "").strip()
        for row in rows
        if not is_closing_status(status_value(row))
    ]


def build_report(workplan_root: Path) -> dict[str, Any]:
    issues: list[str] = []
    for filename in REQUIRED_FILES:
        if not (workplan_root / filename).is_file():
            issues.append(f"validation_plan:missing_file:{filename}")

    plan_path = workplan_root / "VALIDATION_PLAN.md"
    text = plan_path.read_text(encoding="utf-8") if plan_path.is_file() else ""
    if "BETA-DTA-VALIDATION-PLAN" not in text:
        issues.append("validation_plan:missing_search_key")
    if "No step may be skipped" not in text:
        issues.append("validation_plan:missing_no_skip_rule")
    if "server revalidates" not in text:
        issues.append("validation_plan:missing_server_revalidation_boundary")
    if "engine remains SBLR/internal-procedure only" not in text:
        issues.append("validation_plan:missing_sblr_engine_boundary")

    command_rows = load_workplan_csv(workplan_root, "GATE_COMMAND_REGISTRY.csv")
    gate_rows = load_workplan_csv(workplan_root, "ACCEPTANCE_GATES.csv")
    proof_rows = load_workplan_csv(workplan_root, "PROOF_EVIDENCE_LEDGER.csv")
    sequence_rows = load_workplan_csv(workplan_root, "IMPLEMENTATION_SEQUENCE_DAG.csv")

    command_by_id, command_issues = unique_index(
        command_rows, "command_id", "GATE_COMMAND_REGISTRY"
    )
    gate_by_id, gate_issues = unique_index(gate_rows, "gate_id", "ACCEPTANCE_GATES")
    proof_by_id, proof_issues = unique_index(
        proof_rows, "proof_id", "PROOF_EVIDENCE_LEDGER"
    )
    issues.extend(command_issues)
    issues.extend(gate_issues)
    issues.extend(proof_issues)

    for command_id in sorted(REQUIRED_COMMAND_IDS - set(command_by_id)):
        issues.append(f"validation_plan:missing_command_row:{command_id}")
    for gate_id in sorted(REQUIRED_GATE_IDS - set(gate_by_id)):
        issues.append(f"validation_plan:missing_gate_row:{gate_id}")
    for proof_id in sorted(REQUIRED_PROOF_IDS - set(proof_by_id)):
        issues.append(f"validation_plan:missing_proof_row:{proof_id}")

    referenced_commands = ids_in_text(text, "BETA-DTA-CMD")
    for command_id in sorted(REQUIRED_COMMAND_IDS - referenced_commands):
        issues.append(f"validation_plan:command_not_referenced:{command_id}")
    for row in command_rows:
        command_id = row.get("command_id", "").strip()
        command = row.get("command", "").strip()
        artifact = row.get("expected_artifact", "").strip()
        if not command:
            issues.append(f"GATE_COMMAND_REGISTRY:{command_id}:missing_command")
        if not artifact:
            issues.append(f"GATE_COMMAND_REGISTRY:{command_id}:missing_expected_artifact")
        if "future " in command.lower():
            issues.append(f"GATE_COMMAND_REGISTRY:{command_id}:future_command_text")

    incomplete_commands = nonclosing_ids(command_rows, "command_id")
    incomplete_gates = nonclosing_ids(gate_rows, "gate_id")
    incomplete_proofs = nonclosing_ids(proof_rows, "proof_id")
    incomplete_sequence = nonclosing_ids(sequence_rows, "node_id")

    return {
        "command": "driver_beta_validation_plan.py",
        "gate_id": "BETA-DTA-GATE-030",
        "status": report_status(issues + incomplete_commands + incomplete_gates + incomplete_proofs + incomplete_sequence),
        "summary": {
            "command_rows": len(command_rows),
            "gate_rows": len(gate_rows),
            "proof_rows": len(proof_rows),
            "sequence_rows": len(sequence_rows),
            "incomplete_commands": len(incomplete_commands),
            "incomplete_gates": len(incomplete_gates),
            "incomplete_proofs": len(incomplete_proofs),
            "incomplete_sequence_nodes": len(incomplete_sequence),
        },
        "issues": issues,
        "incomplete": {
            "commands": incomplete_commands,
            "gates": incomplete_gates,
            "proofs": incomplete_proofs,
            "sequence_nodes": incomplete_sequence,
        },
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
    print(f"driver_beta_validation_plan={report['status']}")
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
