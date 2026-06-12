#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate SML-049..SML-052 filespace relocation proof fixtures."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
from typing import Any


DEFAULT_OUTPUT_DIR = (
    "project/tests/sbsql_parser_worker/generated/filespace_relocation"
)
MANIFEST_NAME = "SML_049_052_FILESPACE_RELOCATION_MANIFEST.csv"
ORACLE_NAME = "SML_049_052_FILESPACE_RELOCATION_ORACLE.json"
SCHEMA_VERSION = "sbsql.filespace_relocation.sml_049_052.v1"

MANIFEST_COLUMNS = [
    "sml_id",
    "gate_id",
    "row_id",
    "proof_domain",
    "coverage_class",
    "lifecycle_operation",
    "object_class",
    "route_class",
    "page_size",
    "transaction_case",
    "security_case",
    "failure_point",
    "closure_status",
    "proof_kind",
    "parser_executes_sql",
    "parser_owns_finality",
    "storage_finality_authority",
    "recovery_authority",
    "resource_contract",
    "expected_result_contract",
    "expected_result_hash",
    "expected_diagnostic_contract",
    "expected_diagnostic_hash",
    "oracle_id",
    "oracle_hash",
    "artifact_paths",
    "evidence_tokens",
]


def sha256_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def canonical_json(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def row(
    *,
    sml_id: str,
    row_id: str,
    proof_domain: str,
    coverage_class: str,
    lifecycle_operation: str,
    object_class: str,
    route_class: str,
    page_size: str,
    transaction_case: str,
    security_case: str,
    failure_point: str,
    proof_kind: str,
    result: str,
    diagnostic: str,
    resource_contract: str = "network=0;parser_execution=0;external_storage=0",
    recovery_authority: str = "engine_mga_transaction_inventory",
    storage_finality_authority: str = "engine_mga_transaction_inventory",
) -> dict[str, str]:
    gate_id = sml_id.replace("SML-", "SML-GATE-")
    closure_status = "closed_refusal" if proof_kind == "exact_refusal" else "closed_oracle"
    oracle_id = row_id.replace("SML-", "SML-ORACLE-")
    oracle_payload = {
        "sml_id": sml_id,
        "gate_id": gate_id,
        "row_id": row_id,
        "proof_domain": proof_domain,
        "coverage_class": coverage_class,
        "lifecycle_operation": lifecycle_operation,
        "object_class": object_class,
        "route_class": route_class,
        "page_size": page_size,
        "transaction_case": transaction_case,
        "security_case": security_case,
        "failure_point": failure_point,
        "proof_kind": proof_kind,
        "result": result,
        "diagnostic": diagnostic,
        "resource_contract": resource_contract,
        "storage_finality_authority": storage_finality_authority,
        "recovery_authority": recovery_authority,
    }
    return {
        "sml_id": sml_id,
        "gate_id": gate_id,
        "row_id": row_id,
        "proof_domain": proof_domain,
        "coverage_class": coverage_class,
        "lifecycle_operation": lifecycle_operation,
        "object_class": object_class,
        "route_class": route_class,
        "page_size": page_size,
        "transaction_case": transaction_case,
        "security_case": security_case,
        "failure_point": failure_point,
        "closure_status": closure_status,
        "proof_kind": proof_kind,
        "parser_executes_sql": "false",
        "parser_owns_finality": "false",
        "storage_finality_authority": storage_finality_authority,
        "recovery_authority": recovery_authority,
        "resource_contract": resource_contract,
        "expected_result_contract": result,
        "expected_result_hash": sha256_text(result),
        "expected_diagnostic_contract": diagnostic,
        "expected_diagnostic_hash": sha256_text(diagnostic),
        "oracle_id": oracle_id,
        "oracle_hash": sha256_text(canonical_json(oracle_payload)),
        "artifact_paths": (
            "project/tools/sb_parser_gen/generate_sbsql_sml_049_052_filespace_relocation.py;"
            "project/tests/sbsql_parser_worker/generated/filespace_relocation/"
            f"{MANIFEST_NAME};"
            "project/tests/sbsql_parser_worker/generated/filespace_relocation/"
            f"{ORACLE_NAME}"
        ),
        "evidence_tokens": (
            f"{sml_id};{gate_id};{proof_domain};{coverage_class};"
            f"{lifecycle_operation};{object_class};{route_class};"
            "engine_mga_transaction_inventory;network=0;parser_execution=0"
        ),
    }


def manifest_rows() -> list[dict[str, str]]:
    rows = [
        row(
            sml_id="SML-049",
            row_id="SML-049-CREATE-ATTACH-VERIFY-EMBEDDED-8K",
            proof_domain="filespace_lifecycle_manifest",
            coverage_class="lifecycle_operation",
            lifecycle_operation="create",
            object_class="filespace_manifest",
            route_class="embedded",
            page_size="8192",
            transaction_case="autocommit_commit",
            security_case="admin_create_attach",
            failure_point="none",
            proof_kind="oracle",
            result="create records filespace UUID, page size, route, checksum, attach intent, and verify precondition",
            diagnostic="none",
        ),
        row(
            sml_id="SML-049",
            row_id="SML-049-ATTACH-LOCAL-IPC-16K",
            proof_domain="filespace_lifecycle_manifest",
            coverage_class="route",
            lifecycle_operation="attach",
            object_class="filespace_descriptor",
            route_class="local_ipc",
            page_size="16384",
            transaction_case="explicit_commit",
            security_case="attach_requires_open_capability",
            failure_point="capability_epoch_mismatch",
            proof_kind="oracle",
            result="attach binds descriptor only after capability, catalog epoch, and page-size verification pass",
            diagnostic="exact refusal when capability epoch does not match descriptor epoch",
        ),
        row(
            sml_id="SML-049",
            row_id="SML-049-VERIFY-INET-32K",
            proof_domain="filespace_lifecycle_manifest",
            coverage_class="page_size",
            lifecycle_operation="verify",
            object_class="page_map",
            route_class="inet_listener",
            page_size="32768",
            transaction_case="read_snapshot",
            security_case="verify_redacts_paths",
            failure_point="page_hash_mismatch",
            proof_kind="oracle",
            result="verify checks page-map hashes, manifest hashes, route labels, and redacted audit fields",
            diagnostic="exact refusal on page hash mismatch before move admission",
        ),
        row(
            sml_id="SML-049",
            row_id="SML-049-MOVE-PARSER-ROUTE-64K",
            proof_domain="filespace_lifecycle_manifest",
            coverage_class="transaction",
            lifecycle_operation="move",
            object_class="relation_extent",
            route_class="sbsql_parser_route",
            page_size="65536",
            transaction_case="explicit_commit_with_savepoint",
            security_case="move_requires_owner_or_admin",
            failure_point="savepoint_rollback_after_copy",
            proof_kind="oracle",
            result="move keeps old extent visible until committed MGA inventory publishes new extent map",
            diagnostic="none",
        ),
        row(
            sml_id="SML-049",
            row_id="SML-049-PROMOTE-MANAGEMENT-128K",
            proof_domain="filespace_lifecycle_manifest",
            coverage_class="security",
            lifecycle_operation="promote",
            object_class="primary_descriptor",
            route_class="management_api",
            page_size="131072",
            transaction_case="explicit_commit",
            security_case="promotion_requires_dual_control",
            failure_point="dual_control_missing",
            proof_kind="exact_refusal",
            result="not_applicable",
            diagnostic="promotion refused until verified descriptor, blocker-clear evidence, and dual control authorization are present",
        ),
        row(
            sml_id="SML-049",
            row_id="SML-049-DEMOTE-COMPACT-TRUNCATE-RETIRED",
            proof_domain="filespace_lifecycle_manifest",
            coverage_class="failure_point",
            lifecycle_operation="demote",
            object_class="retired_extent",
            route_class="embedded",
            page_size="8192",
            transaction_case="rollback_before_finalize",
            security_case="admin_maintenance",
            failure_point="restart_between_demote_and_compact",
            proof_kind="oracle",
            result="demote, compact, and truncate preserve old-primary reopen evidence until retire is committed",
            diagnostic="restart resumes from manifest phase marker without exposing partially compacted extents",
        ),
        row(
            sml_id="SML-049",
            row_id="SML-049-COMPACT-EXPLICIT-MAINTENANCE",
            proof_domain="filespace_lifecycle_manifest",
            coverage_class="lifecycle_operation",
            lifecycle_operation="compact",
            object_class="free_extent_map",
            route_class="management_api",
            page_size="32768",
            transaction_case="maintenance_commit",
            security_case="compact_requires_admin",
            failure_point="restart_during_extent_rewrite",
            proof_kind="oracle",
            result="compact rewrites only verified free extents and preserves active MGA-visible object versions",
            diagnostic="restart resumes compaction from durable phase marker",
        ),
        row(
            sml_id="SML-049",
            row_id="SML-049-TRUNCATE-EXPLICIT-MAINTENANCE",
            proof_domain="filespace_lifecycle_manifest",
            coverage_class="lifecycle_operation",
            lifecycle_operation="truncate",
            object_class="tail_extent",
            route_class="embedded",
            page_size="65536",
            transaction_case="maintenance_commit",
            security_case="truncate_requires_admin",
            failure_point="active_extent_still_referenced",
            proof_kind="exact_refusal",
            result="not_applicable",
            diagnostic="truncate is refused while any active extent remains visible to an MGA snapshot",
        ),
        row(
            sml_id="SML-049",
            row_id="SML-049-DETACH-EXPLICIT-MAINTENANCE",
            proof_domain="filespace_lifecycle_manifest",
            coverage_class="lifecycle_operation",
            lifecycle_operation="detach",
            object_class="filespace_descriptor",
            route_class="local_ipc",
            page_size="16384",
            transaction_case="explicit_commit",
            security_case="detach_requires_admin",
            failure_point="open_handle_present",
            proof_kind="exact_refusal",
            result="not_applicable",
            diagnostic="detach is refused while verified open handles remain registered",
        ),
        row(
            sml_id="SML-049",
            row_id="SML-049-ARCHIVE-DETACH-DROP",
            proof_domain="filespace_lifecycle_manifest",
            coverage_class="lifecycle_operation",
            lifecycle_operation="archive",
            object_class="archive_package",
            route_class="local_ipc",
            page_size="16384",
            transaction_case="explicit_commit",
            security_case="archive_export_redaction",
            failure_point="drop_before_detach",
            proof_kind="exact_refusal",
            result="archive and detach retain manifest hashes before drop removes descriptor references",
            diagnostic="drop is refused until archive evidence and detach completion are present",
        ),
        row(
            sml_id="SML-050",
            row_id="SML-050-SMALL-SQL-OBJECT-UUID-VERSION",
            proof_domain="sql_object_relocation",
            coverage_class="small_object",
            lifecycle_operation="move",
            object_class="small_sql_table",
            route_class="embedded",
            page_size="8192",
            transaction_case="autocommit_commit",
            security_case="grant_policy_preserved",
            failure_point="none",
            proof_kind="oracle",
            result="small table relocation preserves object UUID, relation version, grants, policies, dependencies, indexes, stats, row results, and metrics",
            diagnostic="none",
        ),
        row(
            sml_id="SML-050",
            row_id="SML-050-LARGE-SQL-OBJECT-LOB-INDEX-STATS",
            proof_domain="sql_object_relocation",
            coverage_class="large_object",
            lifecycle_operation="move",
            object_class="large_sql_object",
            route_class="local_ipc",
            page_size="65536",
            transaction_case="explicit_commit",
            security_case="policy_epoch_preserved",
            failure_point="chunk_copy_restart",
            proof_kind="oracle",
            result="large object relocation preserves UUID trace, versions, large-value chunks, indexes, statistics, result hashes, and metric counters",
            diagnostic="restart resumes chunk copy from durable relocation phase marker",
        ),
        row(
            sml_id="SML-050",
            row_id="SML-050-LOCALIZED-NAMES-DEPENDENCIES",
            proof_domain="sql_object_relocation",
            coverage_class="localized_name",
            lifecycle_operation="verify",
            object_class="localized_name_dependency_graph",
            route_class="inet_listener",
            page_size="32768",
            transaction_case="read_snapshot",
            security_case="name_visibility_policy_preserved",
            failure_point="dependency_hash_mismatch",
            proof_kind="exact_refusal",
            result="localized names and dependency graph hashes are compared before commit",
            diagnostic="relocation refused when localized name, dependency, grant, or policy hash changes",
        ),
        row(
            sml_id="SML-051",
            row_id="SML-051-SHADOW-PROMOTION-BLOCKER-CLEAR",
            proof_domain="primary_shadow_migration",
            coverage_class="promotion",
            lifecycle_operation="promote",
            object_class="shadow_primary",
            route_class="management_api",
            page_size="131072",
            transaction_case="explicit_commit",
            security_case="dual_control_promotion",
            failure_point="blocker_not_clear",
            proof_kind="exact_refusal",
            result="not_applicable",
            diagnostic="shadow primary promotion is refused until new primary verification and blocker-clear evidence are both present",
        ),
        row(
            sml_id="SML-051",
            row_id="SML-051-SHADOW-PROMOTION-VERIFIED-SUCCESS",
            proof_domain="primary_shadow_migration",
            coverage_class="promotion",
            lifecycle_operation="promote",
            object_class="shadow_primary",
            route_class="management_api",
            page_size="131072",
            transaction_case="explicit_commit",
            security_case="dual_control_promotion",
            failure_point="none",
            proof_kind="oracle",
            result="verified new primary promotion succeeds only after blocker-clear evidence, UUID trace continuity, and reopen evidence are recorded",
            diagnostic="none",
        ),
        row(
            sml_id="SML-051",
            row_id="SML-051-OLD-PRIMARY-RETIRE-AFTER-REOPEN",
            proof_domain="primary_shadow_migration",
            coverage_class="retirement",
            lifecycle_operation="retire",
            object_class="old_primary",
            route_class="embedded",
            page_size="8192",
            transaction_case="explicit_commit",
            security_case="retire_requires_admin",
            failure_point="reopen_evidence_missing",
            proof_kind="exact_refusal",
            result="retire keeps old primary attachable for audit until verified reopen evidence exists",
            diagnostic="old primary retirement refused before reopen evidence proves new primary is verified",
        ),
        row(
            sml_id="SML-051",
            row_id="SML-051-DEMOTE-ARCHIVE-DETACH-DROP-ORDER",
            proof_domain="primary_shadow_migration",
            coverage_class="operation_order",
            lifecycle_operation="drop",
            object_class="old_primary_descriptor",
            route_class="local_ipc",
            page_size="16384",
            transaction_case="commit_after_archive",
            security_case="admin_maintenance",
            failure_point="drop_before_archive",
            proof_kind="exact_refusal",
            result="demote, archive, detach, and drop order is enforced after new primary verification",
            diagnostic="drop is refused until old primary is demoted, archived, detached, and no blocker remains",
        ),
        row(
            sml_id="SML-052",
            row_id="SML-052-FAILURE-INJECTION-RESTART-RESUME",
            proof_domain="relocation_failure_recovery",
            coverage_class="restart_resume",
            lifecycle_operation="move",
            object_class="relocation_job",
            route_class="embedded",
            page_size="32768",
            transaction_case="restart_recovery",
            security_case="audit_redaction",
            failure_point="process_restart_after_manifest_commit",
            proof_kind="oracle",
            result="failure injection restart resumes relocation from durable phase marker and preserves UUID trace",
            diagnostic="none",
        ),
        row(
            sml_id="SML-052",
            row_id="SML-052-QUARANTINE-EXACT-REFUSAL",
            proof_domain="relocation_failure_recovery",
            coverage_class="quarantine",
            lifecycle_operation="verify",
            object_class="quarantined_extent",
            route_class="inet_listener",
            page_size="65536",
            transaction_case="rollback_after_fault",
            security_case="operator_exact_refusal",
            failure_point="checksum_fault",
            proof_kind="exact_refusal",
            result="not_applicable",
            diagnostic="checksum fault quarantines the target extent and returns exact refusal with audit event id",
        ),
        row(
            sml_id="SML-052",
            row_id="SML-052-DATAREPAIR-HISTORY-UUID-TRACE",
            proof_domain="relocation_failure_recovery",
            coverage_class="datarepair_history",
            lifecycle_operation="verify",
            object_class="datarepair_history",
            route_class="management_api",
            page_size="131072",
            transaction_case="explicit_commit",
            security_case="repair_history_requires_admin",
            failure_point="repair_after_quarantine",
            proof_kind="oracle",
            result="datarepair history records source UUID, target UUID, phase, refusal id, audit id, and repair decision hash",
            diagnostic="none",
        ),
    ]
    return rows


def oracle_payload(rows: list[dict[str, str]]) -> dict[str, Any]:
    oracle_rows = []
    for item in rows:
        oracle_rows.append(
            {
                "oracle_id": item["oracle_id"],
                "row_id": item["row_id"],
                "sml_id": item["sml_id"],
                "coverage_class": item["coverage_class"],
                "lifecycle_operation": item["lifecycle_operation"],
                "object_class": item["object_class"],
                "route_class": item["route_class"],
                "page_size": item["page_size"],
                "transaction_case": item["transaction_case"],
                "security_case": item["security_case"],
                "failure_point": item["failure_point"],
                "proof_kind": item["proof_kind"],
                "storage_finality_authority": item["storage_finality_authority"],
                "recovery_authority": item["recovery_authority"],
                "expected_result_hash": item["expected_result_hash"],
                "expected_diagnostic_hash": item["expected_diagnostic_hash"],
                "oracle_hash": item["oracle_hash"],
            }
        )
    return {
        "schema_version": SCHEMA_VERSION,
        "manifest": MANIFEST_NAME,
        "fail_closed_statuses": ["closed_oracle", "closed_refusal"],
        "authority": {
            "parser_executes_sql": False,
            "parser_owns_finality": False,
            "storage_finality_authority": "engine_mga_transaction_inventory",
            "recovery_authority": "engine_mga_transaction_inventory",
        },
        "required_lifecycle_operations": [
            "create",
            "attach",
            "verify",
            "move",
            "promote",
            "demote",
            "compact",
            "truncate",
            "retire",
            "archive",
            "detach",
            "drop",
        ],
        "rows": oracle_rows,
    }


def write_manifest(path: Path, rows: list[dict[str, str]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=MANIFEST_COLUMNS, lineterminator="\n")
        writer.writeheader()
        for item in rows:
            writer.writerow(item)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", default=DEFAULT_OUTPUT_DIR)
    args = parser.parse_args()

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    rows = manifest_rows()
    write_manifest(output_dir / MANIFEST_NAME, rows)
    with (output_dir / ORACLE_NAME).open("w", encoding="utf-8") as handle:
        json.dump(oracle_payload(rows), handle, sort_keys=True, indent=2)
        handle.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
