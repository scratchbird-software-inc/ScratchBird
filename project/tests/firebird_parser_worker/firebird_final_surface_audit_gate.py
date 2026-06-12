#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

import argparse
import csv
import sys
from pathlib import Path


def read_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def row_text(row: dict[str, str]) -> str:
    return " ".join(value.lower() for value in row.values() if value)


def validate_proof_closure_claims(
    gate_rows: list[dict[str, str]], agent_rows: list[dict[str, str]]
) -> list[str]:
    errors: list[str] = []
    for row in gate_rows:
        if row.get("status") == "passing_seed":
            errors.append(f"seed-phase CTest status retained after closure: {row.get('gate_name', '<missing>')}")

    active_markers = ("_open", "_pending", "pending_", "still_required")
    for row in agent_rows:
        result = row.get("last_test_result", "")
        if row.get("assignment_status") != "completed" or result not in {
            "passed",
            "docs_validated",
            "manifest_validated",
            "evidence_candidate_recorded",
        }:
            continue
        combined = row_text(row)
        if any(marker in combined for marker in active_markers):
            errors.append(
                "seed-phase completion claim retains follow-up marker: "
                f"{row.get('slice_id', '<missing>')} {row.get('blocker_state', '<missing>')}"
            )
    return errors


def is_final_gate(gate_name: str) -> bool:
    return any(term in gate_name for term in ("final", "exhaustive", "zero_open"))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixture-root", "--execution_plan-root", dest="execution_plan_root", required=True)
    parser.add_argument("--mode", choices=("exhaustive", "final"), required=True)
    args = parser.parse_args()

    artifact_root = Path(args.execution_plan_root).resolve() / "artifacts"
    gate_rows = read_rows(artifact_root / "FIREBIRD_CTEST_REQUIRED_GATES.csv")
    artifact_rows = read_rows(artifact_root / "FIREBIRD_REQUIRED_P0_ARTIFACTS.csv")
    agent_rows = read_rows(artifact_root / "AGENT_EXECUTION_STATUS.csv")

    errors: list[str] = []
    errors.extend(validate_proof_closure_claims(gate_rows, agent_rows))
    for row in gate_rows:
        expected_status = "passing_final" if is_final_gate(row["gate_name"]) else "passing_proof"
        if row["status"] != expected_status:
            errors.append(
                f"required CTest gate status mismatch: "
                f"{row['gate_name']}={row['status']} expected {expected_status}"
            )
    for row in artifact_rows:
        artifact = artifact_root / row["artifact"]
        if row["status"] != "present":
            errors.append(f"required artifact is not present: {row['artifact']}={row['status']}")
        if not artifact.exists():
            errors.append(f"required artifact file missing: {artifact}")

    required_evidence = (
        "FIREBIRD_CTEST_REQUIRED_GATES.csv",
        "FIREBIRD_CTEST_VARIATION_MATRIX.csv",
        "FIREBIRD_REFERENCE_NATIVE_REGRESSION_MANIFEST.csv",
        "FIREBIRD_QA_REFERENCE_REPLAY_MANIFEST.csv",
        "FIREBIRD_REFERENCE_TOOL_BUILD_MANIFEST.csv",
        "AGENT_EXECUTION_STATUS.csv",
    )
    for name in required_evidence:
        if not (artifact_root / name).exists():
            errors.append(f"final audit evidence missing: {name}")

    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    print(f"validated Firebird {args.mode} surface audit")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
