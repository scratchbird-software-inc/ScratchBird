#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate P0 through pre-code execution_plan gates for full SBSQL closure."""

from __future__ import annotations

import argparse
import csv
import re
import sys
from pathlib import Path


ARTIFACTS = Path(__file__).resolve().parent
ROOT = ARTIFACTS.parent
def find_repo_root(start: Path) -> Path:
    for candidate in (start, *start.parents):
        if (candidate / ("." + "git")).exists() and (candidate / "public_release_evidence").exists():
            return candidate
    raise RuntimeError(f"could not find repository root from {start}")


REPO = find_repo_root(ROOT)
CANON = REPO / "public_input_snapshot"


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def status(path: Path) -> str:
    text = path.read_text(encoding="utf-8", errors="replace")
    match = re.search(r"^Status:\s*(.*)$", text, flags=re.M)
    return match.group(1).strip() if match else ""


class GateCheck:
    def __init__(self) -> None:
        self.failures: list[str] = []

    def require(self, condition: bool, message: str) -> None:
        if not condition:
            self.failures.append(message)

    def require_status(self, path: Path, expected: str) -> None:
        actual = status(path)
        self.require(actual == expected, f"{path.relative_to(REPO)} status {actual!r} != {expected!r}")

    def finish(self, gate: str) -> None:
        if self.failures:
            print(f"{gate}: failed")
            for failure in self.failures:
                print(f" - {failure}")
            raise SystemExit(1)
        print(f"{gate}: passed")


def source_rows() -> tuple[list[dict[str, str]], list[dict[str, str]], list[dict[str, str]]]:
    return (
        read_csv(CANON / "SBSQL_SURFACE_REGISTRY.csv"),
        read_csv(CANON / "SBSQL_ENGINE_GAP_MATRIX.csv"),
        read_csv(CANON / "REFERENCE_ALIAS_TO_SBSQL_SURFACE_MATRIX.csv"),
    )


def gate_matrix(check: GateCheck) -> None:
    surfaces, gaps, references = source_rows()
    surface_backlog = read_csv(ARTIFACTS / "SURFACE_IMPLEMENTATION_BACKLOG.csv")
    promotion = read_csv(ARTIFACTS / "NATIVE_FUTURE_PROMOTION_AUDIT.csv")
    gap_backlog = read_csv(ARTIFACTS / "ENGINE_GAP_IMPLEMENTATION_BACKLOG.csv")
    reference_backlog = read_csv(ARTIFACTS / "REFERENCE_ALIAS_COVERAGE_BACKLOG.csv")
    check.require(len(surface_backlog) == len(surfaces), "surface backlog count mismatch")
    check.require(
        len(promotion) == sum(1 for row in surfaces if row["status"] == "native_future"),
        "native_future promotion audit count mismatch",
    )
    check.require(len(gap_backlog) == len(gaps), "engine gap backlog count mismatch")
    check.require(len(reference_backlog) == len(references), "reference alias backlog count mismatch")
    for path in ["MATRIX_COVERAGE_REPORT.md", "NO_DEFER_AUDIT.md"]:
        check.require_status(ARTIFACTS / path, "complete")
    banned = ("defer", "todo", "future", "later", "placeholder")
    for filename in [
        "SURFACE_IMPLEMENTATION_BACKLOG.csv",
        "NATIVE_FUTURE_PROMOTION_AUDIT.csv",
        "ENGINE_GAP_IMPLEMENTATION_BACKLOG.csv",
        "REFERENCE_ALIAS_COVERAGE_BACKLOG.csv",
    ]:
        for index, row in enumerate(read_csv(ARTIFACTS / filename), start=2):
            closure_action = row.get("closure_action", "").lower()
            for word in banned:
                check.require(word not in closure_action, f"{filename}:{index} closure_action contains {word}")


def gate_orchestration(check: GateCheck) -> None:
    tracker = read_csv(ROOT / "TRACKER.csv")
    queue = read_csv(ARTIFACTS / "SLICE_EXECUTION_QUEUE.csv")
    scopes = read_csv(ARTIFACTS / "AGENT_WRITE_SCOPE_REGISTER.csv")
    cadence = read_csv(ARTIFACTS / "VALIDATION_CADENCE_REGISTER.csv")
    failures = read_csv(ARTIFACTS / "FAILURE_INVENTORY.csv")
    check.require_status(ARTIFACTS / "AGENT_ORCHESTRATION_PLAN.md", "complete")
    check.require_status(ARTIFACTS / "AGENT_ORCHESTRATION_AUDIT.md", "complete")
    check.require(len(queue) == len(tracker), "slice queue does not cover every tracker row")
    check.require(all(row.get("validation_command") for row in queue), "queue contains blank validation command")
    check.require(all(row.get("write_scope") for row in queue), "queue contains blank write scope")
    check.require(len(scopes) >= 10, "write-scope register has too few roles")
    check.require(len(cadence) >= 8, "validation cadence register is incomplete")
    check.require(len(failures) == 0, "failure inventory should be empty for passed pre-code gates")


def gate_baseline(check: GateCheck) -> None:
    check.require_status(ARTIFACTS / "BASELINE_BUILD_CAPABILITY_INVENTORY.md", "complete")
    check.require_status(ARTIFACTS / "BASELINE_TEST_RESULT.md", "complete")
    for log in ["configure.log", "build.log", "ctest.log", "target-help.log", "git-status-short.log"]:
        check.require((ARTIFACTS / "baseline" / log).exists(), f"missing baseline log {log}")
    ctest = (ARTIFACTS / "baseline" / "ctest.log").read_text(encoding="utf-8", errors="replace")
    check.require("100% tests passed" in ctest, "baseline ctest did not pass")
    check.require("0 tests failed out of 15" in ctest, "baseline ctest did not record 15/15 pass")


def gate_batching(check: GateCheck) -> None:
    surfaces, _, _ = source_rows()
    batches = read_csv(ARTIFACTS / "REGISTRY_FAMILY_BATCHING_PLAN.csv")
    check.require(sum(int(row["row_count"]) for row in batches) == len(surfaces), "batch row counts do not cover surfaces")
    check.require(all(int(row["row_count"]) <= int(row["max_batch_size"]) for row in batches), "a batch exceeds max size")


def gate_profile(check: GateCheck) -> None:
    path = ARTIFACTS / "FEATURE_PROFILE_CLUSTER_GATE_POLICY.md"
    text = path.read_text(encoding="utf-8", errors="replace")
    check.require_status(path, "complete")
    for token in ["native_now", "native_future", "cluster_private", "policy_blocked", "refused"]:
        check.require(token in text, f"profile policy missing {token}")


def gate_done(check: GateCheck) -> None:
    path = ARTIFACTS / "DEFINITION_OF_DONE_CONTRACT.md"
    text = path.read_text(encoding="utf-8", errors="replace")
    check.require_status(path, "complete")
    for token in ["Parser syntax", "SBLR lowering", "Server route", "Engine behavior", "Diagnostics"]:
        check.require(token in text, f"definition of done missing {token}")


def gate_commands(check: GateCheck) -> None:
    commands = read_csv(ARTIFACTS / "VALIDATION_COMMAND_MATERIALIZATION.csv")
    check.require(len(commands) >= 40, "validation command registry is too small")
    names = {row["command_name"] for row in commands}
    for name in [
        "validation_command_materialization_gate",
        "batch_row_membership_gate",
        "message_vector_seed_gate",
        "resource_budget_policy_gate",
    ]:
        check.require(name in names, f"validation command registry missing {name}")
    for row in commands:
        check.require(row["materialization_status"], f"{row['command_name']} has blank materialization status")
        check.require(row["owning_slice"], f"{row['command_name']} has blank owner")


def gate_membership(check: GateCheck) -> None:
    surfaces, _, _ = source_rows()
    membership = read_csv(ARTIFACTS / "BATCH_ROW_MEMBERSHIP.csv")
    check.require(len(membership) == len(surfaces), "batch membership row count mismatch")
    surface_ids = [row["surface_id"] for row in membership]
    check.require(len(surface_ids) == len(set(surface_ids)), "surface appears in multiple batches")
    check.require({row["surface_id"] for row in surfaces} == set(surface_ids), "batch membership missing a surface")


def gate_oracle(check: GateCheck) -> None:
    surfaces, _, _ = source_rows()
    oracles = read_csv(ARTIFACTS / "SEMANTIC_ORACLE_AUTHORITY_MAP.csv")
    check.require_status(ARTIFACTS / "REGRESSION_FIXTURE_ORACLE_PREPLAN.md", "complete")
    check.require(len(oracles) == len(surfaces), "oracle map row count mismatch")
    check.require(all(row["oracle_source"] and row["expected_result_summary"] for row in oracles), "oracle map has blank authority fields")


def gate_message_vector(check: GateCheck) -> None:
    rows = read_csv(ARTIFACTS / "MESSAGE_VECTOR_COVERAGE_BACKLOG.csv")
    check.require(len(rows) >= 30, "message-vector seed backlog is too small")
    codes = [row["diagnostic_code"] for row in rows]
    check.require(len(codes) == len(set(codes)), "message-vector diagnostic codes are not unique")
    for index, row in enumerate(rows, start=2):
        for key in ["diagnostic_code", "message_vector_fields", "parser_rendering_template", "redaction_policy", "conformance_fixture"]:
            value = row.get(key, "")
            check.require(value and value != "TBD", f"MESSAGE_VECTOR_COVERAGE_BACKLOG.csv:{index} has incomplete {key}")


def gate_authority(check: GateCheck) -> None:
    path = ARTIFACTS / "AUTHORITY_IMPORT_AUDIT.md"
    text = path.read_text(encoding="utf-8", errors="replace")
    check.require_status(path, "complete")
    check.require("not standalone product-behavior authority" in text, "authority audit missing non-normative matrix rule")


def gate_resource(check: GateCheck) -> None:
    path = ARTIFACTS / "RESOURCE_BUDGET_POLICY.md"
    text = path.read_text(encoding="utf-8", errors="replace")
    check.require_status(path, "complete")
    for token in ["SQL statement bytes", "AST depth", "SBLR envelope bytes", "Diagnostic payload bytes"]:
        check.require(token in text, f"resource budget missing {token}")


def gate_cleanup(check: GateCheck) -> None:
    path = ARTIFACTS / "CLEANUP_ARTIFACT_RETENTION_POLICY.md"
    text = path.read_text(encoding="utf-8", errors="replace")
    check.require_status(path, "complete")
    for token in ["Disk Budget Rules", "Retention Classes", "Temporary databases"]:
        check.require(token in text, f"cleanup policy missing {token}")


GATES = {
    "matrix": gate_matrix,
    "orchestration": gate_orchestration,
    "baseline": gate_baseline,
    "batching": gate_batching,
    "profile": gate_profile,
    "done": gate_done,
    "commands": gate_commands,
    "membership": gate_membership,
    "oracle": gate_oracle,
    "message_vector": gate_message_vector,
    "authority": gate_authority,
    "resource": gate_resource,
    "cleanup": gate_cleanup,
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gate", choices=["all", *GATES.keys()], default="all")
    args = parser.parse_args()

    selected = GATES.keys() if args.gate == "all" else [args.gate]
    for gate in selected:
        check = GateCheck()
        GATES[gate](check)
        check.finish(gate)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
