#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate public workload governance and fairness proof anchors."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import sys
from typing import Any


# PUBLIC_WORKLOAD_FAIRNESS_GATE

FORBIDDEN_REFERENCE_FRAGMENTS = (
    "docs" + "/" + "execution-plans",
    "docs" + "/" + "completed-execution-plans",
    "docs" + "/" + "findings",
    "." + "git",
    "/" + "home" + "/",
    "ScratchBird" + "-Private",
    "local" + "_work",
)

CHECKS: tuple[dict[str, Any], ...] = (
    {
        "surface": "resource_seed_lifecycle_fail_closed",
        "path": "project/src/core/resources/resource_seed_pack.hpp",
        "tokens": (
            "WORKLOAD_GOVERNANCE_FAIRNESS",
            "ResourceSeedRuntimeCacheEpoch",
            "missing_seed_refusal_required",
            "unsupported_upgrade_refusal_required",
            "runtime_cache_invalidation_required",
            "index_dependency_current",
        ),
    },
    {
        "surface": "memory_pressure_planner_contract",
        "path": "project/src/core/memory/memory_pressure_response.hpp",
        "tokens": (
            "MEMORY_GOVERNANCE_PRESSURE",
            "MemoryPressureActionKind",
            "throttle",
            "prefer_spill",
            "cancel_query",
            "emergency_admission_shutdown",
            "suspend_noncritical_agents_jobs",
            "recovery_readmission_throttling",
            "adaptive_batch_reduction",
            "forced_spill",
            "forced_cancel",
            "low_priority_query_count",
            "low_priority_session_count",
        ),
    },
    {
        "surface": "memory_pressure_planner_behavior",
        "path": "project/src/core/memory/memory_pressure_response.cpp",
        "tokens": (
            "MMCH_ADAPTIVE_MEMORY_PRESSURE_RESPONSE",
            "CEIC-017_MEMORY_PRESSURE_STATE_MACHINE",
            "memory_pressure.authority_scope=evidence_only_not_transaction_finality_visibility_security_authorization_recovery_parser_reference_wal_benchmark_optimizer_plan_index_finality_or_agent_action_authority",
            "MemoryPressureActionKind::throttle",
            "MemoryPressureActionKind::prefer_spill",
            "MemoryPressureActionKind::forced_spill",
            "MemoryPressureActionKind::cancel_query",
            "MemoryPressureActionKind::suspend_noncritical_agents_jobs",
            "MemoryPressureActionKind::recovery_readmission_throttling",
            "memory_pressure.low_priority_query_count=",
        ),
    },
    {
        "surface": "memory_pressure_executor_gate",
        "path": "project/tests/release/public_memory_pressure_executor_gate.cpp",
        "tokens": (
            "pressure decision missing throttle",
            "pressure decision missing query cancel",
            "pressure decision missing spill preference",
            "pressure decision missing forced spill",
            "pressure decision missing agent suspend",
            "missing pressure executor should fail closed",
            "refused planner decision invoked an executor",
            "public_memory_pressure_executor.agent_suspend_workflow=true",
            "public_memory_pressure_executor.diagnostics_redacted=true",
        ),
    },
    {
        "surface": "query_memory_reservation_gate",
        "path": "project/tests/release/public_query_memory_reservation_gate.cpp",
        "tokens": (
            "ledger current bytes after cancel",
            "arena active grants after cancel",
            "grant should be represented as spill",
            "session_quota_bytes",
            "transaction_quota_bytes",
            "statement_quota_bytes",
            "operation_quota_bytes",
            "allocator failure grant should fail closed",
        ),
    },
    {
        "surface": "hierarchical_ledger_quota_contract",
        "path": "project/src/core/memory/hierarchical_memory_budget_ledger.hpp",
        "tokens": (
            "HierarchicalMemoryScopeKind",
            "tenant",
            "session",
            "transaction",
            "statement",
            "query",
            "operator_scope",
            "HierarchicalMemoryReservationRecommendation",
            "spill",
            "cancel",
            "degrade",
            "priority_weight_total",
        ),
    },
    {
        "surface": "tenant_fairness_scheduler_policy",
        "path": "project/src/core/memory/memory_fairness_scheduler.hpp",
        "tokens": (
            "CEIC-025_MULTI_TENANT_MEMORY_FAIRNESS",
            "MemoryFairnessDecisionAction",
            "grant",
            "spill",
            "throttle",
            "cancel",
            "deny",
            "guarantee_bytes",
            "soft_max_bytes",
            "hard_max_bytes",
            "burst_bytes",
            "starvation_prevention_ms",
            "foreground_protection_bytes",
            "background_scope",
        ),
    },
    {
        "surface": "tenant_fairness_scheduler_behavior",
        "path": "project/src/core/memory/memory_fairness_scheduler.cpp",
        "tokens": (
            "SB-MEMORY-FAIRNESS-HARD-MAX-REFUSED",
            "SB-MEMORY-FAIRNESS-SOFT-MAX-RELIEF",
            "SB-MEMORY-FAIRNESS-FOREGROUND-PROTECTION",
            "SB-MEMORY-FAIRNESS-REQUEST-PROVENANCE-REFUSED",
            "SB-MEMORY-FAIRNESS-CLUSTER-REFUSED",
            "ReliefActionForRequest",
            "memory_fairness.authority_scope",
            "memory_fairness.starvation_prevention.applied=",
            "sb_memory_tenant_fairness_starvation_prevention_total",
        ),
    },
    {
        "surface": "tenant_fairness_stress_gate",
        "path": "project/tests/consolidated_enterprise/ceic_025_multi_tenant_memory_fairness_gate.cpp",
        "tokens": (
            "CEIC-025_MULTI_TENANT_MEMORY_FAIRNESS",
            "HighPriorityTenantProtection",
            "LowPriorityReliefOrdering",
            "BackgroundCannotStarveForeground",
            "TenantCannotBypassThroughSessions",
            "UserRoleSessionLimitsCompose",
            "BurstWindowsExpireAndRefuse",
            "StarvationPreventionEvidence",
            "AuthorityDriftIsRefused",
            "ReleaseApiFailsClosedWithoutLedger",
        ),
    },
    {
        "surface": "tenant_fairness_stress_wiring",
        "path": "project/tests/consolidated_enterprise/CMakeLists.txt",
        "tokens": (
            "ceic_025_multi_tenant_memory_fairness_gate",
            "CEIC-GATE-011",
            "tenant_fairness",
            "foreground_protection",
            "spill_throttle_cancel",
            "authority_boundary",
        ),
    },
    {
        "surface": "release_gate_wiring",
        "path": "project/tests/release/CMakeLists.txt",
        "tokens": (
            "public_memory_pressure_executor_gate",
            "public_query_memory_reservation_gate",
            "public_workload_fairness_gate",
        ),
    },
)


def fail(message: str) -> None:
    print(f"public_workload_fairness_gate=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def reject_private_reference(value: str, context: str) -> None:
    if Path(value).is_absolute():
        fail(f"absolute_path_recorded:{context}:{value}")
    for fragment in FORBIDDEN_REFERENCE_FRAGMENTS:
        if fragment in value:
            fail(f"private_reference_recorded:{context}:{value}")


def read_text(repo_root: Path, relative_path: str) -> str:
    reject_private_reference(relative_path, "source_path")
    path = repo_root / relative_path
    require(path.is_file(), f"source_missing:{relative_path}")
    try:
        return path.read_text(encoding="utf-8")
    except UnicodeDecodeError as exc:
        fail(f"utf8_decode_failed:{relative_path}:{exc}")
    except OSError as exc:
        fail(f"read_failed:{relative_path}:{exc}")


def validate_check(repo_root: Path, check: dict[str, Any]) -> dict[str, Any]:
    surface = check["surface"]
    path_text = check["path"]
    tokens = check["tokens"]
    require(isinstance(surface, str) and surface, "surface_invalid")
    require(isinstance(path_text, str) and path_text, f"path_invalid:{surface}")
    require(isinstance(tokens, tuple) and tokens, f"tokens_invalid:{surface}")
    text = read_text(repo_root, path_text)
    token_digests: list[str] = []
    for token in tokens:
        require(isinstance(token, str) and token, f"token_invalid:{surface}")
        if token not in text:
            fail(f"token_missing:{surface}:{path_text}:{token}")
        token_digests.append(sha256_text(token))
    return {
        "surface": surface,
        "path": path_text,
        "token_count": len(tokens),
        "source_sha256": sha256_text(text),
        "token_digest_sha256": sha256_text("\n".join(token_digests) + "\n"),
        "status": "pass",
    }


def write_csv(path: Path, records: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=[
                "surface",
                "path",
                "token_count",
                "source_sha256",
                "token_digest_sha256",
                "status",
            ],
        )
        writer.writeheader()
        writer.writerows(records)


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--csv-output", required=True, type=Path)
    parser.add_argument("--evidence-output", required=True, type=Path)
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    repo_root = args.repo_root.resolve()
    records = [validate_check(repo_root, check) for check in CHECKS]
    matrix_text = "\n".join(
        f"{record['surface']},{record['path']},{record['status']}" for record in records
    ) + "\n"
    payload = {
        "gate": "PUBLIC_WORKLOAD_FAIRNESS_GATE",
        "status": "pass",
        "checks": records,
        "check_count": len(records),
        "matrix_sha256": sha256_text(matrix_text),
        "authority": {
            "memory_governance_evidence_only": True,
            "transaction_finality": "local_mga_transaction_inventory",
            "workload_governance_is_transaction_authority": False,
            "workload_governance_is_security_authority": False,
            "release_pressure_gate": "public_memory_pressure_executor_gate",
            "reservation_gate": "public_query_memory_reservation_gate",
            "tenant_fairness_gate": "ceic_025_multi_tenant_memory_fairness_gate",
        },
    }
    write_csv(args.csv_output, records)
    write_json(args.evidence_output, payload)
    print(
        "public_workload_fairness_gate=passed "
        f"checks={len(records)} matrix_sha256={payload['matrix_sha256']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
