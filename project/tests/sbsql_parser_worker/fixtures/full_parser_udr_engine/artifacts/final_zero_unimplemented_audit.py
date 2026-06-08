#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""FSPE-014 final zero-unimplemented audit gate."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path


ARTIFACTS = Path("project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/artifacts")

EXPECTED_COUNTS = {
    "public_input_snapshot": 2617,
    "public_input_snapshot": 2617,
    "public_input_snapshot": 932,
    "public_input_snapshot": 312,
    ARTIFACTS / "SURFACE_IMPLEMENTATION_BACKLOG.csv": 2617,
    ARTIFACTS / "BATCH_ROW_MEMBERSHIP.csv": 2617,
    ARTIFACTS / "SEMANTIC_ORACLE_AUTHORITY_MAP.csv": 2617,
    ARTIFACTS / "ENGINE_GAP_IMPLEMENTATION_BACKLOG.csv": 932,
    ARTIFACTS / "DONOR_ALIAS_COVERAGE_BACKLOG.csv": 312,
}

REQUIRED_STATUS_REPORTS = (
    ARTIFACTS / "MATRIX_COVERAGE_REPORT.md",
    ARTIFACTS / "NO_DEFER_AUDIT.md",
    ARTIFACTS / "PARSER_COVERAGE_REPORT.md",
    ARTIFACTS / "UDR_COVERAGE_REPORT.md",
    ARTIFACTS / "SERVER_ENGINE_GAP_CLOSURE_REPORT.md",
    ARTIFACTS / "SPEC_SYNCHRONIZATION_AUDIT.md",
    ARTIFACTS / "DEVELOPER_HANDOFF_IMPLEMENTATION_MAP.csv",
    ARTIFACTS / "SOURCE_SIZE_MAINTAINABILITY_REPORT.md",
    ARTIFACTS / "ZERO_SQL_ENGINE_BOUNDARY_AUDIT.md",
    ARTIFACTS / "FULL_REGRESSION_SUITE_PUBLICATION.md",
    ARTIFACTS / "KNOWN_RISK_BURN_DOWN_REPORT.md",
    ARTIFACTS / "CLEANUP_ARTIFACT_RETENTION_POLICY.md",
    ARTIFACTS / "BENCHMARK_BASELINE_REPORT.md",
    ARTIFACTS / "EXHAUSTIVE_E2E_REGRESSION_REPORT.md",
    ARTIFACTS / "CANARY_VERTICAL_SLICE_PLAN.md",
    ARTIFACTS / "CANARY_VERTICAL_SLICE_RESULT.md",
)

FINAL_REPORTS = (
    ARTIFACTS / "FSPE_014F_VALIDATION_RESULT.md",
    ARTIFACTS / "FSPE_014G_VALIDATION_RESULT.md",
    ARTIFACTS / "BASELINE_TEST_RESULT.md",
)

BANNED_CLOSURE_WORDS = (
    "defer",
    "todo",
    "future",
    "later",
    "placeholder",
    "stub",
    "parser-only",
    "spec-only",
)


class Gate:
    def __init__(self) -> None:
        self.failures: list[str] = []

    def require(self, condition: bool, message: str) -> None:
        if not condition:
            self.failures.append(message)

    def finish(self) -> int:
        if self.failures:
            print("final_zero_unimplemented_audit: failed")
            for failure in self.failures:
                print(f" - {failure}")
            return 1
        print("final_zero_unimplemented_audit: passed")
        return 0


def read_rows(repo: Path, rel: str | Path) -> list[dict[str, str]]:
    path = repo / rel
    with path.open(newline="", encoding="utf-8", errors="replace") as handle:
        return list(csv.DictReader(handle))


def row_count(repo: Path, rel: str | Path) -> int:
    return len(read_rows(repo, rel))


def has_status_complete(repo: Path, rel: Path) -> bool:
    text = (repo / rel).read_text(encoding="utf-8", errors="replace")
    return "Status: complete" in text


def has_terminal_status(repo: Path, rel: Path) -> bool:
    text = (repo / rel).read_text(encoding="utf-8", errors="replace")
    return "Status: complete" in text or "Status: passed" in text


def check_all_statuses(repo: Path, gate: Gate) -> None:
    for rel, status_field in (
        (ARTIFACTS / "SLICE_EXECUTION_QUEUE.csv", "status"),
    ):
        for row in read_rows(repo, rel):
            gate.require(
                row.get(status_field) == "complete",
                f"{rel} row is not complete: {row}",
            )

    for row in read_rows(repo, ARTIFACTS / "VALIDATION_COMMAND_MATERIALIZATION.csv"):
        gate.require(row.get("materialization_status") == "complete", f"validation command not complete: {row}")
        gate.require(row.get("runnable_now") == "yes", f"validation command not runnable: {row}")
        gate.require(
            "owning slice must provide" not in row.get("executable_or_contract", ""),
            f"future validation contract remains: {row}",
        )


def check_counts(repo: Path, gate: Gate) -> None:
    for rel, expected in EXPECTED_COUNTS.items():
        actual = row_count(repo, rel)
        gate.require(actual == expected, f"{rel} row count {actual} != {expected}")


def check_closure_actions(repo: Path, gate: Gate) -> None:
    for rel in (
        ARTIFACTS / "SURFACE_IMPLEMENTATION_BACKLOG.csv",
        ARTIFACTS / "ENGINE_GAP_IMPLEMENTATION_BACKLOG.csv",
        ARTIFACTS / "DONOR_ALIAS_COVERAGE_BACKLOG.csv",
    ):
        for row in read_rows(repo, rel):
            action = row.get("closure_action", "").lower()
            for banned in BANNED_CLOSURE_WORDS:
                gate.require(banned not in action, f"{rel} banned closure word {banned!r}: {row}")


def check_engine_gap_closure(repo: Path, gate: Gate) -> None:
    rows = read_rows(repo, ARTIFACTS / "ENGINE_GAP_IMPLEMENTATION_BACKLOG.csv")
    noncluster = [row for row in rows if row.get("cluster_scope") != "cluster_private"]
    cluster = [row for row in rows if row.get("cluster_scope") == "cluster_private"]
    gate.require(len(noncluster) == 816, f"non-cluster engine gap count changed: {len(noncluster)}")
    gate.require(len(cluster) == 116, f"cluster-private engine gap count changed: {len(cluster)}")
    gate.require(
        all(row.get("status") == "closed_by_engine_api_sblr_family_gate" for row in noncluster),
        "one or more non-cluster engine gaps are not closed by engine API/SBLR family gate",
    )
    gate.require(
        all(row.get("status") == "closed_by_cluster_fail_closed_gate" for row in cluster),
        "one or more cluster-private engine gaps are not closed by cluster fail-closed gate",
    )


def check_reports(repo: Path, gate: Gate) -> None:
    for rel in REQUIRED_STATUS_REPORTS:
        path = repo / rel
        gate.require(path.exists(), f"required report missing: {rel}")
        if path.suffix == ".md" and path.exists():
            gate.require(has_status_complete(repo, rel), f"required report is not complete: {rel}")
            text = path.read_text(encoding="utf-8", errors="replace")
            gate.require("will be populated" not in text, f"placeholder text remains in {rel}")

    for rel in FINAL_REPORTS:
        path = repo / rel
        gate.require(path.exists(), f"final report missing: {rel}")
        if path.exists():
            text = path.read_text(encoding="utf-8", errors="replace")
            gate.require(has_terminal_status(repo, rel), f"final report is not terminal: {rel}")


def check_authority_and_boundary(repo: Path, gate: Gate) -> None:
    gate.require((repo / "public_contract_snapshot").exists(), "canonical spec manifest missing")
    gate.require((repo / "public_contract_snapshot").exists(), "canonical authority file missing")

    for rel, token in (
        (ARTIFACTS / "ZERO_SQL_ENGINE_BOUNDARY_AUDIT.md", "Engine executes SBLR/internal procedures only"),
        (ARTIFACTS / "ZERO_SQL_ENGINE_BOUNDARY_AUDIT.md", "MGA remains Alpha recovery authority"),
        (ARTIFACTS / "ZERO_SQL_ENGINE_BOUNDARY_AUDIT.md", "No raw SQL text is admitted as engine execution authority"),
        (ARTIFACTS / "KNOWN_RISK_BURN_DOWN_REPORT.md", "No WAL recovery or SQL-in-engine authority is accepted"),
        (ARTIFACTS / "FSPE_014G_VALIDATION_RESULT.md", "sbsql_exhaustive_e2e_regression"),
    ):
        text = (repo / rel).read_text(encoding="utf-8", errors="replace")
        gate.require(token in text, f"{rel} missing authority/boundary token: {token}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path.cwd())
    args = parser.parse_args()

    repo = args.repo_root.resolve()
    gate = Gate()
    check_all_statuses(repo, gate)
    check_counts(repo, gate)
    check_closure_actions(repo, gate)
    check_engine_gap_closure(repo, gate)
    check_reports(repo, gate)
    check_authority_and_boundary(repo, gate)
    return gate.finish()


if __name__ == "__main__":
    raise SystemExit(main())
