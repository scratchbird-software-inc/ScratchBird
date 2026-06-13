#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate the reference parser implementation-start gate evidence controller.

This is a no-go enforcement gate for the current evidence state. It proves that
the central reference parser gate package is explicit, source-independent, and
cannot be mistaken for parser implementation-start approval while blocking
evidence is still open.
"""

from __future__ import annotations

import argparse
import csv
import pathlib
import sys


EXECUTION_PLAN_ROOT = pathlib.Path("docs") / "execution-plans"
EXECUTION_PLAN = EXECUTION_PLAN_ROOT / "reference-parser-implementation-gate-evidence-closure"
REGRESSION_PLAN = EXECUTION_PLAN_ROOT / "reference-parser-regression-policy-extraction-closure"
EXTERNAL_CONTROLLER_SKIP_CODE = 77

EVIDENCE_FILES = {
    "reference_regression_inventory": "upstream_manifest.csv",
    "native_tool_replay": "native_tool_harness/native_tool_harness_manifest.csv",
    "security_policy": "security_operations/security_policy_manifest.csv",
    "catalog_policy": "catalog_policy/catalog_policy_manifest.csv",
    "migration_policy": "operations_migration/migration_policy_manifest.csv",
    "performance_baseline": "performance/performance_baseline_manifest.csv",
    "wire_transcripts": "wire_transcripts/wire_transcript_manifest.csv",
    "resource_limits": "resource_limits/resource_limit_manifest.csv",
    "management_package_abi": "management_package_abi/management_package_abi_manifest.csv",
    "release_evidence": "release_evidence/release_evidence_manifest.csv",
}


def read_csv(repo: pathlib.Path, rel: pathlib.Path) -> list[dict[str, str]]:
    path = repo / rel
    if not path.is_file():
        raise AssertionError(f"missing required CSV: {rel}")
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        rows: list[dict[str, str]] = []
        for row in reader:
            row.pop(None, None)
            rows.append(dict(row))
    if not rows:
        raise AssertionError(f"{rel}: no rows")
    return rows


def require_file(repo: pathlib.Path, rel: pathlib.Path) -> pathlib.Path:
    path = repo / rel
    if not path.is_file():
        raise AssertionError(f"missing required file: {rel}")
    return path


def require_text(repo: pathlib.Path, rel: pathlib.Path, token: str) -> None:
    text = require_file(repo, rel).read_text(encoding="utf-8")
    if token not in text:
        raise AssertionError(f"{rel}: missing token {token!r}")


def unique_values(rows: list[dict[str, str]], column: str) -> set[str]:
    values = {row.get(column, "") for row in rows}
    if "" in values:
        raise AssertionError(f"{column}: empty value present")
    return values


def is_generated_or_proof_closed_status(status: str) -> bool:
    return (
        "manifest_generated" in status
        or "locator_corrected" in status
        or status in {"completed", "passed", "passing", "no_exclusions_accepted"}
        or status.endswith("_passed")
        or status.endswith("_closed")
    )


def validate_no_go_controller(repo: pathlib.Path) -> None:
    required_files = (
        EXECUTION_PLAN / "ACCEPTED_START_EVIDENCE_REGISTER.csv",
        EXECUTION_PLAN / "EVIDENCE_BLOCKER_REGISTER.csv",
        EXECUTION_PLAN / "CENTRAL_EVIDENCE_VALIDATION_REPORT.md",
        EXECUTION_PLAN / "IMPLEMENTATION_START_HANDOFF_PACKET.md",
        EXECUTION_PLAN / "VALIDATION_SUMMARY.json",
        EXECUTION_PLAN / "GO_NO_GO.md",
    )
    for rel in required_files:
        require_file(repo, rel)

    require_text(repo, EXECUTION_PLAN / "GO_NO_GO.md", "no_go_pending_evidence_execution_and_gold_signoff")
    require_text(
        repo,
        EXECUTION_PLAN / "IMPLEMENTATION_START_HANDOFF_PACKET.md",
        "no_go_pending_evidence_execution_and_gold_signoff",
    )

    blockers = read_csv(repo, EXECUTION_PLAN / "EVIDENCE_BLOCKER_REGISTER.csv")
    if len(blockers) < 8:
        raise AssertionError("blocker register must enumerate the active gate blockers")
    nonblocking = [row["blocker_id"] for row in blockers if row.get("status") != "blocking"]
    if nonblocking:
        raise AssertionError(f"blocker rows not marked blocking: {', '.join(nonblocking)}")

    accepted = read_csv(repo, EXECUTION_PLAN / "ACCEPTED_START_EVIDENCE_REGISTER.csv")
    required_accepted = {
        "DPGEC-ACC-001",
        "DPGEC-ACC-002",
        "DPGEC-ACC-003",
        "DPGEC-ACC-004",
        "DPGEC-ACC-005",
        "DPGEC-ACC-006",
        "DPGEC-ACC-007",
    }
    actual_accepted = unique_values(accepted, "evidence_id")
    missing = sorted(required_accepted - actual_accepted)
    if missing:
        raise AssertionError(f"accepted evidence register missing rows: {', '.join(missing)}")

    ledger = read_csv(repo, EXECUTION_PLAN / "REFERENCE_GATE_EVIDENCE_LEDGER.csv")
    if len(ledger) != 25:
        raise AssertionError(f"reference ledger must have 25 parser rows, found {len(ledger)}")
    bad_ledger = [
        row["reference_id"]
        for row in ledger
        if row.get("implementation_start_state") != "no_go_pending_central_evidence"
        or row.get("status") != "blocked_by_gate_evidence_register"
    ]
    if bad_ledger:
        raise AssertionError(f"ledger rows not held at no-go: {', '.join(bad_ledger)}")

    require_file(repo, pathlib.Path("project/tests/reference_regression/generate_reference_parser_start_evidence.py"))
    missing_evidence: list[str] = []
    bad_evidence_status: list[str] = []
    for row in ledger:
        reference_id = row["reference_id"]
        root = pathlib.Path(row["project_tests_root"])
        for family, rel in EVIDENCE_FILES.items():
            evidence_rel = root / rel
            try:
                evidence_rows = read_csv(repo, evidence_rel)
            except AssertionError:
                missing_evidence.append(f"{reference_id}:{family}:{evidence_rel.as_posix()}")
                continue
            statuses = {evidence_row.get("status", "") for evidence_row in evidence_rows}
            if not any(is_generated_or_proof_closed_status(status) for status in statuses):
                bad_evidence_status.append(f"{reference_id}:{family}:{','.join(sorted(statuses))}")
    if missing_evidence:
        raise AssertionError(
            "missing generated reference parser start evidence manifests: "
            + "; ".join(missing_evidence[:20])
        )
    if bad_evidence_status:
        raise AssertionError(
            "invalid reference parser start evidence manifest statuses: "
            + "; ".join(bad_evidence_status[:20])
        )

    gates = read_csv(repo, EXECUTION_PLAN / "ACCEPTANCE_GATES.csv")
    if any(row.get("status") == "pending" for row in gates):
        raise AssertionError("central acceptance gates must not remain generically pending")
    if not any(row.get("status", "").startswith("blocked_") for row in gates):
        raise AssertionError("no-go controller must retain explicit blocked gate statuses")

    project_map = read_csv(repo, EXECUTION_PLAN / "PROJECT_TEST_EVIDENCE_MAP.csv")
    if any(row.get("status") != "manifest_generated_pending_execution" for row in project_map):
        raise AssertionError("project test evidence map must record generated manifests pending execution")

    qa = read_csv(repo, EXECUTION_PLAN / "QA_AUDIT_SIGNOFF_MATRIX.csv")
    if any(row.get("status") != "blocked_pending_required_inputs" for row in qa):
        raise AssertionError("QA/audit rows must remain blocked until required inputs exist")

    sources = read_csv(repo, REGRESSION_PLAN / "REFERENCE_REGRESSION_SOURCE_LOCATION_MATRIX.csv")
    if len(sources) != 25:
        raise AssertionError(f"source locator matrix must have 25 parser rows, found {len(sources)}")
    if any(row.get("source_status") != "verified_present" for row in sources):
        not_verified = [row["reference_id"] for row in sources if row.get("source_status") != "verified_present"]
        raise AssertionError(f"unverified reference source locators: {', '.join(not_verified)}")

    for rel in (
        REGRESSION_PLAN / "REFERENCE_NATIVE_TEST_TOOL_INDEX.csv",
        REGRESSION_PLAN / "PROJECT_TEST_DESTINATION_MATRIX.csv",
    ):
        rows = read_csv(repo, rel)
        values = unique_values(rows, "reference_id")
        if len(values) != 25:
            raise AssertionError(f"{rel}: expected 25 distinct reference_id values, found {len(values)}")
        for reference_id in ("opensearch_rest", "opensearch_sql_ppl"):
            if reference_id not in values:
                raise AssertionError(f"{rel}: missing normalized {reference_id} row")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=pathlib.Path)
    args = parser.parse_args(argv)
    repo = args.repo_root.resolve()
    if not (repo / EXECUTION_PLAN).is_dir():
        print(f"missing external controller evidence directory: {EXECUTION_PLAN}", file=sys.stderr)
        return EXTERNAL_CONTROLLER_SKIP_CODE
    validate_no_go_controller(repo)
    print("reference_parser_gate_evidence_no_go=pass")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
