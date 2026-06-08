#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Standalone DPC-008 failure-injection crash matrix gate.

Search key: DPC_FAILURE_INJECTION_GATE

This is a contract regression gate for planned persisted/background DPC
surfaces. It validates complete enumeration, fail-closed classification,
message-vector keys, and owned fixture routes. It does not execute live
crash/reopen recovery for feature structures that do not exist yet.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any


MATRIX_KEY = "DPC_FAILURE_INJECTION_CRASH_MATRIX"
GATE_KEY = "DPC_FAILURE_INJECTION_GATE"
GATE_NAME = "dpc_failure_injection_crash_matrix_gate"
SCHEMA_VERSION = "dpc.failure_injection_crash_matrix.v1"
FIXTURE_ROUTE_PREFIX = "fixture://database_lifecycle/dpc_failure_injection/"
MESSAGE_VECTOR_PREFIX = "DPC.FAILURE."
REQUIRED_LABELS = (
    "DPC-008",
    "DPC_TEST_FAILURE_INJECTION",
    "failure_injection",
    "crash_recovery",
    "mga_transaction_regression",
    "database_lifecycle",
)
REQUIRED_SURFACES = {
    "deferred_index_delta": 2,
    "index_delta_merge": 1,
    "shadow_index": 3,
    "page_summary": 1,
    "mga_cleanup": 1,
    "index_cleanup": 1,
    "search_segment": 2,
    "vector_generation": 2,
    "storage": 2,
    "server": 1,
}
ALLOWED_PROOF_SCOPES = {
    "contract_crash_reopen_classification",
    "contract_failure_injection_classification",
}
ALLOWED_REQUIRED_TESTS = {
    "standalone CTest crash reopen",
    "standalone CTest failure injection",
}
EXPECTED_RUNTIME_POLICY = {
    "reads_execution_plan_files": False,
    "proof_scope": "contract_matrix",
    "actual_crash_reopen_execution": False,
    "requires_engine_feature_implementation": False,
    "transaction_authority": "scratchbird_mga_transaction_inventory",
    "recovery_authority": "scratchbird_mga_recovery_classification",
}
EXPECTED_ROWS: tuple[dict[str, str], ...] = (
    {
        "failure_id": "DPC_FAIL_001",
        "surface": "deferred_index_delta",
        "classification": "deferred_delta_precommit",
        "crash_or_failure_point": "after delta ledger append before transaction commit",
        "expected_recovery_or_refusal": "rollback ignores or removes uncommitted delta and index remains safe",
        "message_vector_key": "DPC.FAILURE.DEFERRED_INDEX_DELTA.UNCOMMITTED_DELTA_ROLLBACK_SAFE",
        "fixture_route": "fixture://database_lifecycle/dpc_failure_injection/deferred_index_delta/pre_commit_delta_append",
        "required_test": "standalone CTest crash reopen",
        "proof_scope": "contract_crash_reopen_classification",
    },
    {
        "failure_id": "DPC_FAIL_002",
        "surface": "deferred_index_delta",
        "classification": "deferred_delta_postcommit_premerge",
        "crash_or_failure_point": "after transaction commit before delta merge",
        "expected_recovery_or_refusal": "reader overlay sees committed delta or index use is refused with exact message",
        "message_vector_key": "DPC.FAILURE.DEFERRED_INDEX_DELTA.COMMITTED_DELTA_OVERLAY_OR_REFUSAL",
        "fixture_route": "fixture://database_lifecycle/dpc_failure_injection/deferred_index_delta/post_commit_pre_merge",
        "required_test": "standalone CTest crash reopen",
        "proof_scope": "contract_crash_reopen_classification",
    },
    {
        "failure_id": "DPC_FAIL_003",
        "surface": "index_delta_merge",
        "classification": "merge_batch_pre_publish",
        "crash_or_failure_point": "during merge batch before publish",
        "expected_recovery_or_refusal": "merge resumes idempotently or leaves old base plus delta overlay safe",
        "message_vector_key": "DPC.FAILURE.INDEX_DELTA_MERGE.BATCH_PRE_PUBLISH_SAFE",
        "fixture_route": "fixture://database_lifecycle/dpc_failure_injection/index_delta_merge/batch_pre_publish",
        "required_test": "standalone CTest crash reopen",
        "proof_scope": "contract_crash_reopen_classification",
    },
    {
        "failure_id": "DPC_FAIL_004",
        "surface": "shadow_index",
        "classification": "shadow_build_pre_validation",
        "crash_or_failure_point": "during shadow build before validation",
        "expected_recovery_or_refusal": "shadow index remains invisible and repair can discard or rebuild",
        "message_vector_key": "DPC.FAILURE.SHADOW_INDEX.BUILD_PRE_VALIDATION_INVISIBLE",
        "fixture_route": "fixture://database_lifecycle/dpc_failure_injection/shadow_index/build_pre_validation",
        "required_test": "standalone CTest crash reopen",
        "proof_scope": "contract_crash_reopen_classification",
    },
    {
        "failure_id": "DPC_FAIL_005",
        "surface": "shadow_index",
        "classification": "shadow_drain_pre_publish",
        "crash_or_failure_point": "during delta drain before publish barrier",
        "expected_recovery_or_refusal": "old index remains visible and shadow state is repairable or refused",
        "message_vector_key": "DPC.FAILURE.SHADOW_INDEX.DELTA_DRAIN_PRE_PUBLISH_SAFE",
        "fixture_route": "fixture://database_lifecycle/dpc_failure_injection/shadow_index/delta_drain_pre_publish",
        "required_test": "standalone CTest crash reopen",
        "proof_scope": "contract_crash_reopen_classification",
    },
    {
        "failure_id": "DPC_FAIL_006",
        "surface": "shadow_index",
        "classification": "shadow_publish_barrier",
        "crash_or_failure_point": "during publish barrier",
        "expected_recovery_or_refusal": "exactly one index generation is visible after reopen or restricted-open repair is required",
        "message_vector_key": "DPC.FAILURE.SHADOW_INDEX.PUBLISH_BARRIER_SINGLE_GENERATION",
        "fixture_route": "fixture://database_lifecycle/dpc_failure_injection/shadow_index/publish_barrier",
        "required_test": "standalone CTest crash reopen",
        "proof_scope": "contract_crash_reopen_classification",
    },
    {
        "failure_id": "DPC_FAIL_007",
        "surface": "page_summary",
        "classification": "summary_rebuild_stale_fallback",
        "crash_or_failure_point": "during summary rebuild",
        "expected_recovery_or_refusal": "summary is marked stale and planner falls back to full scan",
        "message_vector_key": "DPC.FAILURE.PAGE_SUMMARY.REBUILD_STALE_FULL_SCAN_FALLBACK",
        "fixture_route": "fixture://database_lifecycle/dpc_failure_injection/page_summary/rebuild_stale_fallback",
        "required_test": "standalone CTest crash reopen",
        "proof_scope": "contract_crash_reopen_classification",
    },
    {
        "failure_id": "DPC_FAIL_008",
        "surface": "mga_cleanup",
        "classification": "mga_cleanup_horizon",
        "crash_or_failure_point": "during storage version cleanup",
        "expected_recovery_or_refusal": "no version visible to active transaction is removed and cleanup horizon is recalculated",
        "message_vector_key": "DPC.FAILURE.MGA_CLEANUP.ACTIVE_VISIBILITY_HORIZON_RECALCULATED",
        "fixture_route": "fixture://database_lifecycle/dpc_failure_injection/mga_cleanup/storage_version_cleanup",
        "required_test": "standalone CTest crash reopen",
        "proof_scope": "contract_crash_reopen_classification",
    },
    {
        "failure_id": "DPC_FAIL_009",
        "surface": "index_cleanup",
        "classification": "index_cleanup_validation",
        "crash_or_failure_point": "during batched index garbage cleanup",
        "expected_recovery_or_refusal": "index validation detects incomplete cleanup and resumes or refuses unsafe use",
        "message_vector_key": "DPC.FAILURE.INDEX_CLEANUP.INCOMPLETE_BATCH_VALIDATE_OR_REFUSE",
        "fixture_route": "fixture://database_lifecycle/dpc_failure_injection/index_cleanup/batched_garbage_cleanup",
        "required_test": "standalone CTest crash reopen",
        "proof_scope": "contract_crash_reopen_classification",
    },
    {
        "failure_id": "DPC_FAIL_010",
        "surface": "search_segment",
        "classification": "search_segment_publish",
        "crash_or_failure_point": "during inverted segment publish",
        "expected_recovery_or_refusal": "old exact fallback remains valid and unpublished segment is discarded or repaired",
        "message_vector_key": "DPC.FAILURE.SEARCH_SEGMENT.PUBLISH_EXACT_FALLBACK",
        "fixture_route": "fixture://database_lifecycle/dpc_failure_injection/search_segment/inverted_segment_publish",
        "required_test": "standalone CTest crash reopen",
        "proof_scope": "contract_crash_reopen_classification",
    },
    {
        "failure_id": "DPC_FAIL_011",
        "surface": "search_segment",
        "classification": "search_segment_merge",
        "crash_or_failure_point": "during segment merge",
        "expected_recovery_or_refusal": "old segments remain visible until merge commit marker is complete",
        "message_vector_key": "DPC.FAILURE.SEARCH_SEGMENT.MERGE_OLD_SEGMENTS_VISIBLE",
        "fixture_route": "fixture://database_lifecycle/dpc_failure_injection/search_segment/segment_merge",
        "required_test": "standalone CTest crash reopen",
        "proof_scope": "contract_crash_reopen_classification",
    },
    {
        "failure_id": "DPC_FAIL_012",
        "surface": "vector_generation",
        "classification": "vector_build_before_seal",
        "crash_or_failure_point": "during vector build before seal",
        "expected_recovery_or_refusal": "vector index is not admitted and exact scan fallback remains available",
        "message_vector_key": "DPC.FAILURE.VECTOR_GENERATION.BUILD_BEFORE_SEAL_EXACT_FALLBACK",
        "fixture_route": "fixture://database_lifecycle/dpc_failure_injection/vector_generation/build_before_seal",
        "required_test": "standalone CTest crash reopen",
        "proof_scope": "contract_crash_reopen_classification",
    },
    {
        "failure_id": "DPC_FAIL_013",
        "surface": "vector_generation",
        "classification": "vector_generation_publish",
        "crash_or_failure_point": "during sealed generation publish",
        "expected_recovery_or_refusal": "old vector generation or exact fallback remains available and unsafe generation is refused",
        "message_vector_key": "DPC.FAILURE.VECTOR_GENERATION.SEALED_PUBLISH_REFUSE_UNSAFE",
        "fixture_route": "fixture://database_lifecycle/dpc_failure_injection/vector_generation/sealed_generation_publish",
        "required_test": "standalone CTest crash reopen",
        "proof_scope": "contract_crash_reopen_classification",
    },
    {
        "failure_id": "DPC_FAIL_014",
        "surface": "storage",
        "classification": "storage_disk_full",
        "crash_or_failure_point": "disk full during ledger summary shadow or segment write",
        "expected_recovery_or_refusal": "operation fails with exact message and transaction state remains coherent",
        "message_vector_key": "DPC.FAILURE.STORAGE.DISK_FULL_TRANSACTION_COHERENT",
        "fixture_route": "fixture://database_lifecycle/dpc_failure_injection/storage/disk_full",
        "required_test": "standalone CTest failure injection",
        "proof_scope": "contract_failure_injection_classification",
    },
    {
        "failure_id": "DPC_FAIL_015",
        "surface": "storage",
        "classification": "storage_allocation_failure",
        "crash_or_failure_point": "allocation failure during DML or cleanup",
        "expected_recovery_or_refusal": "operation fails or throttles without visibility drift",
        "message_vector_key": "DPC.FAILURE.STORAGE.ALLOCATION_FAILURE_NO_VISIBILITY_DRIFT",
        "fixture_route": "fixture://database_lifecycle/dpc_failure_injection/storage/allocation_failure",
        "required_test": "standalone CTest failure injection",
        "proof_scope": "contract_failure_injection_classification",
    },
    {
        "failure_id": "DPC_FAIL_016",
        "surface": "server",
        "classification": "server_agent_kill",
        "crash_or_failure_point": "server kill during agent action",
        "expected_recovery_or_refusal": "reopen classifies action state and support bundle records recovery decision",
        "message_vector_key": "DPC.FAILURE.SERVER.AGENT_KILL_RECOVERY_DECISION",
        "fixture_route": "fixture://database_lifecycle/dpc_failure_injection/server/agent_action_kill",
        "required_test": "standalone CTest crash reopen",
        "proof_scope": "contract_crash_reopen_classification",
    },
)


class GateError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise GateError(message)


def load_fixture(path: Path) -> dict[str, Any]:
    resolved = path.resolve()
    lower_parts = {part.lower() for part in resolved.parts}
    require(not ({"docs", "execution-plans"} <= lower_parts), f"fixture path must not be a execution_plan path: {resolved}")
    require(not ({"docs", "completed-execution-plans"} <= lower_parts), f"fixture path must not be a completed execution_plan path: {resolved}")
    try:
        return json.loads(resolved.read_text(encoding="utf-8"))
    except OSError as exc:
        raise GateError(f"could not read fixture {resolved}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise GateError(f"fixture is not valid JSON: {exc}") from exc


def require_exact_top_level(fixture: dict[str, Any]) -> None:
    require(fixture.get("schema_version") == SCHEMA_VERSION, "schema_version mismatch")
    require(fixture.get("matrix_key") == MATRIX_KEY, "matrix_key mismatch")
    require(fixture.get("gate_key") == GATE_KEY, "gate_key mismatch")
    require(fixture.get("gate_name") == GATE_NAME, "gate_name mismatch")
    require(tuple(fixture.get("required_labels", ())) == REQUIRED_LABELS, "required_labels mismatch")
    require(fixture.get("runtime_policy") == EXPECTED_RUNTIME_POLICY, "runtime_policy mismatch")


def require_exact_rows(rows: list[dict[str, Any]]) -> None:
    require(len(rows) == len(EXPECTED_ROWS), f"expected {len(EXPECTED_ROWS)} rows, found {len(rows)}")
    by_id = {row.get("failure_id"): row for row in rows}
    expected_ids = tuple(f"DPC_FAIL_{i:03d}" for i in range(1, 17))
    require(tuple(by_id.keys()) == expected_ids, f"failure IDs must be ordered and exact: {tuple(by_id.keys())}")

    seen_message_keys: set[str] = set()
    seen_fixture_routes: set[str] = set()
    surface_counts: dict[str, int] = {}
    for expected in EXPECTED_ROWS:
        failure_id = expected["failure_id"]
        row = by_id.get(failure_id)
        require(row is not None, f"missing {failure_id}")
        require(row == expected, f"{failure_id} contract mismatch: {row!r}")
        require(row["proof_scope"] in ALLOWED_PROOF_SCOPES, f"{failure_id} invalid proof scope")
        require(row["required_test"] in ALLOWED_REQUIRED_TESTS, f"{failure_id} invalid required_test")
        require(row["message_vector_key"].startswith(MESSAGE_VECTOR_PREFIX), f"{failure_id} message key prefix mismatch")
        require(row["message_vector_key"] not in seen_message_keys, f"{failure_id} duplicate message-vector key")
        require(row["fixture_route"].startswith(FIXTURE_ROUTE_PREFIX), f"{failure_id} fixture route must be database_lifecycle-owned")
        require(row["fixture_route"] not in seen_fixture_routes, f"{failure_id} duplicate fixture route")
        seen_message_keys.add(row["message_vector_key"])
        seen_fixture_routes.add(row["fixture_route"])
        surface_counts[row["surface"]] = surface_counts.get(row["surface"], 0) + 1

    require(surface_counts == REQUIRED_SURFACES, f"surface coverage mismatch: {surface_counts}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", required=True, help="Project-owned DPC failure injection fixture JSON")
    args = parser.parse_args()

    try:
        fixture = load_fixture(Path(args.fixture))
        require_exact_top_level(fixture)
        rows = fixture.get("rows")
        require(isinstance(rows, list), "rows must be a list")
        require_exact_rows(rows)
    except GateError as exc:
        print(f"{GATE_KEY}=failed: {exc}", file=sys.stderr)
        return 1

    print(
        f"{GATE_KEY}=passed rows={len(EXPECTED_ROWS)} "
        "proof_scope=contract_matrix actual_crash_reopen_execution=false"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
