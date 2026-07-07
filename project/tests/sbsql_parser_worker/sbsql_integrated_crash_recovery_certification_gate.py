#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Close SBSQL-MISS-016 integrated proof and crash/recovery evidence."""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path


SURFACE_ROOT = Path("project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts")
CMAKE_FILES = (
    Path("project/tests/sbsql_parser_worker/CMakeLists.txt"),
    Path("project/tests/mga_transaction_regression/CMakeLists.txt"),
    Path("project/tests/fault_injection/CMakeLists.txt"),
)

REQUIRED_CTESTS = {
    "sbsql_persistence_restart_conformance",
    "sbsql_stream_finality_conformance",
    "sbsql_concurrent_session_transaction_conformance",
    "transaction_inventory_publish_fault_conformance",
    "mga_transaction_regression_inventory_visibility",
    "mga_transaction_regression_savepoint_rollback",
    "mga_transaction_regression_lock_concurrency",
    "mga_transaction_regression_recovery_classification",
    "mga_transaction_regression_stress_invariant",
    "mga_transaction_regression_authority_policy_gate",
    "mga_transaction_regression_benchmark_storage_policy_gate",
    "mga_transaction_regression_engine_api_e2e",
    "mga_transaction_regression_single_node_inventory",
    "mga_transaction_regression_recovery_probe",
    "mga_transaction_regression_driver_reconciliation",
    "sbsql_missing_functionality_lock_mga_policy_conformance",
    "sbsql_transaction_policy_closure_conformance",
    "sbsql_dml_mga_row_result_conformance",
}

GENERATED_MGA_CASES = {
    "inventory_visibility",
    "savepoint_rollback",
    "lock_concurrency",
    "recovery_classification",
    "stress_invariant",
}

REQUIRED_SOURCE_TOKENS = {
    Path("project/tests/sbsql_parser_worker/sbsql_persistence_restart_conformance.cpp"): (
        "startup_recovery_classification == \"clean_checkpoint_path\"",
        "typed_catalog_records_present",
        "resource_seed_catalog",
        "RequireDescriptorContains",
        "index_lookup",
        "contains_sql_text=false",
        "parser_resolved_names_to_uuids=true",
    ),
    Path("project/tests/sbsql_parser_worker/sbsql_stream_finality_conformance.cpp"): (
        "stream_finality_mode",
        "timed_out",
        "cancelled",
        "parser_killed",
        "cursor_uuid",
    ),
    Path("project/tests/sbsql_parser_worker/sbsql_concurrent_session_transaction_conformance.cpp"): (
        "target_table_not_visible",
        "transaction.create_savepoint",
        "transaction.rollback_to_savepoint",
        "PARSER_SERVER_IPC.PREPARED_STATEMENT_NOT_FOUND",
        "PARSER_SERVER_IPC.CURSOR_NOT_FOUND",
        "parser_finality:false",
    ),
    Path("project/src/engine/internal_api/catalog/descriptor_api.cpp"): (
        "catalog_object_fallback",
        "EngineGetDescriptorUncachedImpl",
    ),
    Path("project/src/engine/internal_api/dml/insert_physical_integration.cpp"): (
        "metadata_event_sequence",
        "CurrentMgaRelationMetadataEventSequence(context)",
        "DirectLookupBulkAppendContextCache",
        "DirectStoreBulkAppendContextCache",
        "DirectStoreAppendIndexEntryCache",
    ),
    Path("project/src/engine/internal_api/mga_relation_store/mga_relation_store.cpp"): (
        "CurrentMgaRelationMetadataEventSequence",
        "ScanNextMetadataEventSequence(context)",
    ),
    Path("project/src/engine/internal_api/mga_relation_store/mga_relation_store.hpp"): (
        "CurrentMgaRelationMetadataEventSequence",
    ),
}

REQUIRED_SURFACE_TOKENS = (
    "parser_executes_sql=false",
    "sql_text_included=false",
    "no_wal_authority",
    "canonical_message_vector_set",
)


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        return path.read_text(encoding="utf-8", errors="ignore")


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def validate_ctest_registration(repo: Path) -> list[str]:
    errors: list[str] = []
    cmake_text = "\n".join(read_text(repo / path) for path in CMAKE_FILES)
    for test_name in sorted(REQUIRED_CTESTS):
        if test_name.startswith("mga_transaction_regression_"):
            case_name = test_name.removeprefix("mga_transaction_regression_")
            if case_name in GENERATED_MGA_CASES:
                continue
        if test_name not in cmake_text:
            errors.append(f"required CTest is not registered: {test_name}")
    mga_cmake = read_text(repo / CMAKE_FILES[1])
    if 'NAME "mga_transaction_regression_${regression_case}"' not in mga_cmake:
        errors.append("MGA regression CMake no longer generates case-specific CTest names")
    for case_name in sorted(GENERATED_MGA_CASES):
        if case_name not in mga_cmake:
            errors.append(f"MGA regression case is not registered: {case_name}")
    parser_cmake = read_text(repo / CMAKE_FILES[0])
    if "SBSQL-MISS-GATE-016" not in parser_cmake:
        errors.append("CMake is missing the SBSQL-MISS-GATE-016 label")
    if "sbsql_integrated_crash_recovery_certification_gate" not in parser_cmake:
        errors.append("CMake is missing the SBSQL-MISS-016 aggregate gate")
    return errors


def validate_source_invariants(repo: Path) -> list[str]:
    errors: list[str] = []
    for rel, tokens in REQUIRED_SOURCE_TOKENS.items():
        path = repo / rel
        if not path.is_file():
            errors.append(f"missing source proof file: {rel}")
            continue
        text = read_text(path)
        for token in tokens:
            if token not in text:
                errors.append(f"{rel}: missing proof token {token!r}")
    return errors


def validate_surface_evidence(repo: Path) -> tuple[list[str], int]:
    errors: list[str] = []
    evidence_path = repo / SURFACE_ROOT / "PER_ROW_EVIDENCE_MANIFEST.csv"
    release_path = repo / SURFACE_ROOT / "SBSQL_SURFACE_RELEASE_DECLARATION.csv"
    evidence_rows = read_csv(evidence_path)
    release_rows = read_csv(release_path)
    if len(evidence_rows) != len(release_rows):
        errors.append(
            f"surface evidence/release row count mismatch: {len(evidence_rows)} != {len(release_rows)}"
        )
    if len(evidence_rows) < 2500:
        errors.append(f"surface evidence row count below expected floor: {len(evidence_rows)}")

    evidence_ids = {row["surface_id"] for row in evidence_rows}
    release_ids = {row["surface_id"] for row in release_rows}
    if evidence_ids != release_ids:
        errors.append(
            "surface evidence/release id mismatch: "
            f"missing_release={len(evidence_ids - release_ids)} missing_evidence={len(release_ids - evidence_ids)}"
        )

    allowed_states = {"e2e_passed", "cluster_provider_route_passed"}
    for index, row in enumerate(evidence_rows, start=2):
        if row["final_state"] not in allowed_states:
            errors.append(f"evidence row {index}: final_state not closed: {row['final_state']}")
        joined = ";".join(row.get(column, "") for column in row)
        if row.get("category") == "transaction":
            for token in REQUIRED_SURFACE_TOKENS:
                if token not in joined:
                    errors.append(f"transaction evidence row {index}: missing {token}")

    for index, row in enumerate(release_rows, start=2):
        if row["final_status"] not in allowed_states:
            errors.append(f"release row {index}: final_status not closed: {row['final_status']}")
        if row["release_status"] != "row_evidence_complete":
            errors.append(f"release row {index}: release_status not complete: {row['release_status']}")
        if row["remaining_risk"] != "none":
            errors.append(f"release row {index}: remaining risk is not zero: {row['remaining_risk']}")

    return errors, len(evidence_rows)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()

    repo = Path(args.repo_root).resolve()
    try:
        errors = []
        errors.extend(validate_ctest_registration(repo))
        errors.extend(validate_source_invariants(repo))
        surface_errors, surface_row_count = validate_surface_evidence(repo)
        errors.extend(surface_errors)
    except Exception as exc:  # noqa: BLE001 - gate output should be explicit.
        print("sbsql_integrated_crash_recovery_certification_gate=failed", file=sys.stderr)
        print(str(exc), file=sys.stderr)
        return 2

    if errors:
        print("sbsql_integrated_crash_recovery_certification_gate=failed", file=sys.stderr)
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    print("sbsql_integrated_crash_recovery_certification_gate=passed")
    print(f"required_ctests={len(REQUIRED_CTESTS)}")
    print(f"surface_evidence_rows={surface_row_count}")
    print(f"release_declaration_rows={surface_row_count}")
    print("descriptor_restart_proof=rich_descriptor_before_generic_catalog_fallback")
    print("direct_insert_index_cache_proof=metadata_event_sequence_invalidated")
    print("engine_authority=SBLR_UUID_MGA_only")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
