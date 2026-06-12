#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Master gate for the final SBLR/SBsql closure sequence."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path
import sys


MASTER_ROOT = Path("public_execution_plan")
CHILDREN = {
    "inventory": Path("public_execution_plan"),
    "cluster": Path("public_execution_plan"),
    "sblr": Path("public_execution_plan"),
    "parser": Path("public_execution_plan"),
    "sbsql": Path("public_execution_plan"),
    "proof": Path("public_execution_plan"),
}

EXPECTED_MASTER = {
    "FSM-P0": "sequence_locked",
    "FSM-P1": "dependency_dag_locked",
    "FSM-P2": "inventory_child_closed",
    "FSM-P3": "cluster_child_closed",
    "FSM-P4": "sblr_child_closed",
    "FSM-P5": "parser_and_sbsql_children_closed",
    "FSM-P6": "enterprise_proof_child_closed",
    "FSM-P7": "final_audit_handoff_packet_ready",
}


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def read_csv(root: Path, rel: Path) -> list[dict[str, str]]:
    path = root / rel
    try:
        with path.open(newline="", encoding="utf-8") as handle:
            rows = list(csv.DictReader(handle))
    except FileNotFoundError:
        fail(f"missing CSV: {rel}")
    require(rows, f"CSV has no rows: {rel}")
    return rows


def keyed(rows: list[dict[str, str]], column: str, label: str) -> dict[str, dict[str, str]]:
    out: dict[str, dict[str, str]] = {}
    for row in rows:
        value = row.get(column, "")
        require(value, f"{label} missing {column}")
        require(value not in out, f"{label} duplicate {column}: {value}")
        out[value] = row
    return out


def validate_child_closed(root: Path, name: str, child: Path) -> None:
    tracker = read_csv(root, child / "TRACKER.csv")
    gates = read_csv(root, child / "ACCEPTANCE_GATES.csv")
    for row in tracker:
        require(row["status"] != "pending", f"{name} tracker still pending: {row}")
    for row in gates:
        require(row["status"] != "pending", f"{name} gate still pending: {row}")


def validate_master(root: Path) -> None:
    tracker = keyed(read_csv(root, MASTER_ROOT / "TRACKER.csv"), "slice_id", "master tracker")
    for key, status in EXPECTED_MASTER.items():
        require(tracker[key]["status"] == status, f"{key} status drift: {tracker[key]['status']}")
    gates = read_csv(root, MASTER_ROOT / "ACCEPTANCE_GATES.csv")
    for row in gates:
        require(row["status"] != "pending", f"{row['gate_id']} still pending")
    exit_rows = read_csv(root, MASTER_ROOT / "MASTER_EXIT_CRITERIA.csv")
    for row in exit_rows:
        require(row["status"] == "exit_criterion_verified",
                f"exit criterion not verified: {row['criterion_id']}")
    handoff = read_csv(root, MASTER_ROOT / "MASTER_AUDIT_HANDOFF_MATRIX.csv")
    for row in handoff:
        require(row["status"] != "pending", f"audit handoff still pending: {row['handoff_id']}")


def validate_cross_child_counts(root: Path) -> None:
    sblr = read_csv(root, CHILDREN["sblr"] / "SBLR_EXECUTION_PROOF_MATRIX.csv")
    sbsql = read_csv(root, CHILDREN["sbsql"] / "SBSQL_TO_SBLR_PROOF_MATRIX.csv")
    roundtrip = read_csv(root, CHILDREN["proof"] / "SBLR_SBSQL_ROUNDTRIP_PROOF_MATRIX.csv")
    parser_audit = read_csv(root, CHILDREN["parser"] / "PARSER_DIALECT_ISOLATION_AUDIT.csv")
    cluster = read_csv(root, CHILDREN["cluster"] / "NORMALIZED_REFERENCE_CLUSTER_COMMAND_SET.csv")
    require(len(sblr) == 2760, "SBLR proof count drift")
    require(len(sbsql) == 2760, "SBsql proof count drift")
    require(len(roundtrip) == 2760, "roundtrip proof count drift")
    require(len(parser_audit) == 89, "parser audit count drift")
    require(len(cluster) == 59, "cluster command count drift")
    sblr_ops = {row["operation_id"] for row in sblr}
    require({row["feature"] for row in sbsql} == sblr_ops,
            "SBsql proof feature set does not match SBLR operation set")
    require({row["surface"] for row in roundtrip} == sblr_ops,
            "enterprise roundtrip feature set does not match SBLR operation set")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()
    root = Path(args.repo_root).resolve()

    for name, child in CHILDREN.items():
        validate_child_closed(root, name, child)
    validate_master(root)
    validate_cross_child_counts(root)
    print("final_sblr_sbsql_master_closure_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
