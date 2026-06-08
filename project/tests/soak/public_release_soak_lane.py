#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Bounded public release soak lane for PCR-116."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys
from typing import Any


# PUBLIC_RELEASE_SOAK_LANE

SOAK_ROWS: tuple[dict[str, Any], ...] = (
    {
        "row_id": "memory_pressure",
        "gate": "public_memory_pressure_executor_gate",
        "time_budget_seconds": 30,
        "iteration_limit": 1,
        "artifact": "public_memory_pressure_executor_gate_tmp",
        "evidence_files": {
            "tests/release/public_memory_pressure_executor_gate.cpp": (
                "public_memory_pressure_executor_gate",
                "hard_limit_bytes = 1000",
                "pressure decision missing agent suspend",
                "pressure decision missing diagnostics",
                "missing pressure executor should fail closed",
            ),
        },
    },
    {
        "row_id": "memory_concurrency_reference",
        "gate": "memory_sanitizer_soak_concurrency_gate",
        "time_budget_seconds": 60,
        "iteration_limit": 512,
        "artifact": "scratchbird_mmch_soak_workspace",
        "evidence_files": {
            "tests/concurrency/memory_sanitizer_soak_concurrency_gate.cpp": (
                "MMCH_MEMORY_SANITIZER_SOAK_CONCURRENCY",
                "constexpr int kThreads = 8",
                "constexpr int kIterations = 512",
                "concurrent allocation churn had failures",
                "memory leak after concurrent allocation churn",
            ),
        },
    },
    {
        "row_id": "concurrent_transactions",
        "gate": "public_transaction_inventory_lock_table_gate",
        "time_budget_seconds": 30,
        "iteration_limit": 4,
        "artifact": "public_transaction_inventory_lock_table_gate_tmp",
        "evidence_files": {
            "tests/release/public_transaction_inventory_lock_table_gate.cpp": (
                "concurrent lock smoke should complete",
                "concurrent lock smoke should release all locks",
                "concurrent lock smoke should leave no waiters",
                "upgrade deadlock should use stable diagnostic",
                "held-lock admission limit should fail closed",
            ),
        },
    },
    {
        "row_id": "cleanup_sweep",
        "gate": "public_transaction_savepoint_limbo_cleanup_gate",
        "time_budget_seconds": 30,
        "iteration_limit": 8,
        "artifact": "public_transaction_savepoint_limbo_cleanup_gate_tmp",
        "evidence_files": {
            "tests/release/public_transaction_savepoint_limbo_cleanup_gate.cpp": (
                "max_candidate_row_versions = 8",
                "PCR-072 bounded sweep should preserve reclaim evidence",
                "PCR-072 reclaim evidence limit should fail closed",
                "PCR-072 non-engine cleanup sweep should fail closed",
                "cleanup evidence must not claim physical storage mutation",
            ),
        },
    },
    {
        "row_id": "index_maintenance",
        "gate": "public_index_durable_metadata_validator_gate",
        "time_budget_seconds": 30,
        "iteration_limit": 1,
        "artifact": "public_index_readiness_matrix_gate_artifacts",
        "evidence_files": {
            "tests/release/public_index_readiness_matrix_gate.cpp": (
                "public_index_readiness_matrix_gate",
                "recovery_reopen",
                "validate_repair_route_complete",
                "durable_closure_claimed",
            ),
            "tests/release/public_index_durable_metadata_validator_gate.cpp": (
                "artifact planner refused valid metadata",
                "artifact planner accepted tampered durable metadata",
                "family validator claimed engine authority",
            ),
        },
    },
    {
        "row_id": "backup_forward",
        "gate": "public_backup_forward_session_gate",
        "time_budget_seconds": 60,
        "iteration_limit": 2,
        "artifact": "public_backup_forward_session_gate_tmp",
        "evidence_files": {
            "tests/release/public_backup_forward_session_gate.cpp": (
                "backup-forward session should start",
                "write-after segment must remain non-authority",
                "delta apply should be idempotent",
                "coverage gap should fail closed",
            ),
            "tests/release/public_backup_update_coverage_gate.cpp": (
                "idempotent update should reuse existing segment only",
                "PITR target outside coverage should fail",
                "PITR_TARGET_OUTSIDE_COVERAGE",
            ),
        },
    },
    {
        "row_id": "agents",
        "gate": "public_agent_readiness_matrix_gate",
        "time_budget_seconds": 30,
        "iteration_limit": 29,
        "artifact": "public_agent_readiness_matrix_gate_artifacts",
        "evidence_files": {
            "tests/release/public_agent_readiness_matrix_gate.cpp": (
                "CanonicalAgentManifestCount() == 29",
                "matrix missing dry-run evaluator status",
                "matrix missing workflow ledger actuator-required status",
                "action contract owner missing from matrix",
            ),
            "tests/agents/agent_fault_injection_gate.cpp": (
                "AgentFaultInjectionScenarioDescriptors",
                "restart_mid_action",
                "partial_page_preallocation",
            ),
        },
    },
    {
        "row_id": "support_bundle_generation",
        "gate": "public_transaction_support_bundle_gate",
        "time_budget_seconds": 30,
        "iteration_limit": 1,
        "artifact": "transaction_support_bundle_summary",
        "evidence_files": {
            "tests/release/public_transaction_support_bundle_gate.cpp": (
                "support bundle should prepare",
                "support bundle should apply redaction",
                "support_bundle_is_authority",
                "support_bundle_transaction_evidence",
                "authority-claiming support bundle should fail closed",
            ),
        },
    },
)


def fail(message: str) -> None:
    print(f"public_release_soak_lane=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def require_file(project_root: Path, relative: str) -> str:
    path = project_root / relative
    if not path.is_file():
        fail(f"missing_file:{relative}")
    return path.read_text(encoding="utf-8")


def require_tokens(text: str, relative: str, tokens: tuple[str, ...]) -> list[str]:
    missing = [token for token in tokens if token not in text]
    if missing:
        fail(f"missing_tokens:{relative}:{','.join(missing)}")
    return list(tokens)


def build_evidence(project_root: Path) -> dict[str, Any]:
    if project_root.name != "project" or not project_root.is_dir():
        fail("project_root_must_be_project_directory")

    rows: list[dict[str, Any]] = []
    seen_ids: set[str] = set()
    for row in SOAK_ROWS:
        row_id = row["row_id"]
        if row_id in seen_ids:
            fail(f"duplicate_row:{row_id}")
        seen_ids.add(row_id)
        if row["time_budget_seconds"] <= 0 or row["iteration_limit"] <= 0:
            fail(f"unbounded_row:{row_id}")
        file_records: list[dict[str, Any]] = []
        for relative, tokens in row["evidence_files"].items():
            text = require_file(project_root, relative)
            file_records.append(
                {
                    "path": relative,
                    "sha256": sha256_text(text),
                    "required_tokens": require_tokens(text, relative, tokens),
                }
            )
        rows.append(
            {
                "row_id": row_id,
                "gate": row["gate"],
                "time_budget_seconds": row["time_budget_seconds"],
                "iteration_limit": row["iteration_limit"],
                "artifact": row["artifact"],
                "file_count": len(file_records),
                "files": file_records,
            }
        )

    return {
        "schema_version": 1,
        "gate": "PCR-GATE-116",
        "marker": "PUBLIC_RELEASE_SOAK_LANE",
        "policy": {
            "bounded": True,
            "deterministic": True,
            "private_docs_required": False,
            "diagnostic_artifacts_required": True,
            "unbounded_soak_required_for_gate": False,
        },
        "row_count": len(rows),
        "total_time_budget_seconds": sum(row["time_budget_seconds"] for row in rows),
        "rows": rows,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    evidence = build_evidence(Path(args.project_root).resolve())
    output = Path(args.output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n",
                      encoding="utf-8")
    print(
        "public_release_soak_lane=passed "
        f"rows={evidence['row_count']} budget={evidence['total_time_budget_seconds']}s "
        f"output={output.name}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
