#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate the public diagnostic stability matrix."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import sys
from typing import Any


# PUBLIC_DIAGNOSTIC_MATRIX_GENERATOR

FORBIDDEN_REFERENCE_FRAGMENTS = (
    "docs" + "/" + "execution-plans",
    "docs" + "/" + "completed-execution-plans",
    "docs" + "/" + "findings",
    "." + "git",
    "/" + "home" + "/",
    "ScratchBird" + "-Private",
    "local" + "_work",
)

REQUIRED_AREAS = {
    "storage",
    "mga",
    "security",
    "optimizer",
    "agents",
    "indexes",
    "cluster_boundary",
    "release_gate",
    "unsupported_feature",
    "diagnostics",
}

REDACTION_CLASSES = {
    "public_code_only",
    "redacted_detail",
    "metadata_only",
    "secret_free",
    "path_free",
}

COMPATIBILITY_STATUSES = {
    "stable_public",
    "fail_closed_stable",
    "unsupported_stable",
    "native_runner_required",
    "diagnostic_only_stable",
}

MATRIX_ROWS: tuple[dict[str, str], ...] = (
    {
        "diagnostic_id": "storage.format.future_minor",
        "area": "storage",
        "code": "FORMAT.VERSION_UNSUPPORTED",
        "message_key": "format.version_unsupported",
        "redaction_class": "public_code_only",
        "compatibility_status": "fail_closed_stable",
        "source_path": "src/storage/disk/database_format.cpp",
        "source_token": "FORMAT.VERSION_UNSUPPORTED",
        "public_test_path": "tools/release/public_release_version_metadata.cpp",
        "public_test_token": "FORMAT.VERSION_UNSUPPORTED",
    },
    {
        "diagnostic_id": "storage.catalog_page.future_format",
        "area": "storage",
        "code": "SB-CATALOG-PAGE-BODY-FORMAT-UNSUPPORTED",
        "message_key": "catalog.page_body.format_unsupported",
        "redaction_class": "public_code_only",
        "compatibility_status": "fail_closed_stable",
        "source_path": "src/storage/page/catalog_page.cpp",
        "source_token": "SB-CATALOG-PAGE-BODY-FORMAT-UNSUPPORTED",
        "public_test_path": "tests/release/public_codec_property_gate.cpp",
        "public_test_token": "SB-CATALOG-PAGE-BODY-FORMAT-UNSUPPORTED",
    },
    {
        "diagnostic_id": "mga.physical_sweep.authority_required",
        "area": "mga",
        "code": "SB-ROW-DATA-PHYSICAL-SWEEP-MGA-AUTHORITY-REQUIRED",
        "message_key": "row_data.physical_sweep.mga_authority_required",
        "redaction_class": "public_code_only",
        "compatibility_status": "fail_closed_stable",
        "source_path": "src/storage/page/row_data_physical_sweep.cpp",
        "source_token": "SB-ROW-DATA-PHYSICAL-SWEEP-MGA-AUTHORITY-REQUIRED",
        "public_test_path": "tests/release/public_mga_physical_sweep_gate.cpp",
        "public_test_token": "SB-ROW-DATA-PHYSICAL-SWEEP-MGA-AUTHORITY-REQUIRED",
    },
    {
        "diagnostic_id": "mga.historical_snapshot.authority_evidence_required",
        "area": "mga",
        "code": "SB-MGA-HISTORICAL-SNAPSHOT-AUTHORITY-EVIDENCE-REQUIRED",
        "message_key": "mga.historical_snapshot.authority_evidence_required",
        "redaction_class": "metadata_only",
        "compatibility_status": "fail_closed_stable",
        "source_path": "src/transaction/mga/historical_snapshot_locator.cpp",
        "source_token": "SB-MGA-HISTORICAL-SNAPSHOT-AUTHORITY-EVIDENCE-REQUIRED",
        "public_test_path": "tests/release/public_mga_historical_snapshot_locator_gate.cpp",
        "public_test_token": "SB-MGA-HISTORICAL-SNAPSHOT-AUTHORITY-EVIDENCE-REQUIRED",
    },
    {
        "diagnostic_id": "security.authorization.denied",
        "area": "security",
        "code": "SECURITY.AUTHORIZATION.DENIED",
        "message_key": "security.authorization.denied",
        "redaction_class": "redacted_detail",
        "compatibility_status": "fail_closed_stable",
        "source_path": "src/engine/internal_api/security/security_model.cpp",
        "source_token": "SECURITY.AUTHORIZATION.DENIED",
        "public_test_path": "tests/release/public_authorization_durable_flow_gate.cpp",
        "public_test_token": "SECURITY.AUTHORIZATION.DENIED",
    },
    {
        "diagnostic_id": "unsupported.auth_provider.unsupported",
        "area": "unsupported_feature",
        "code": "SECURITY.AUTH_PROVIDER_UNSUPPORTED",
        "message_key": "security.auth_provider.unsupported",
        "redaction_class": "redacted_detail",
        "compatibility_status": "unsupported_stable",
        "source_path": "src/engine/internal_api/security/auth_provider_model.cpp",
        "source_token": "SECURITY.AUTH_PROVIDER_UNSUPPORTED",
        "public_test_path": "tests/release/public_security_provider_contract_protected_material_gate.cpp",
        "public_test_token": "SECURITY.AUTH_PROVIDER_UNSUPPORTED",
    },
    {
        "diagnostic_id": "optimizer.catalog.index_stats_required",
        "area": "optimizer",
        "code": "SB_OPT_CATALOG_BACKED_PLANNING.INDEX_STATS_REQUIRED",
        "message_key": "optimizer.catalog_backed_planning.index_stats_required",
        "redaction_class": "metadata_only",
        "compatibility_status": "fail_closed_stable",
        "source_path": "src/engine/optimizer/optimizer_catalog_backed_planning.cpp",
        "source_token": "SB_OPT_CATALOG_BACKED_PLANNING.INDEX_STATS_REQUIRED",
        "public_test_path": "tests/release/public_optimizer_catalog_backed_planning_gate.cpp",
        "public_test_token": "SB_OPT_CATALOG_BACKED_PLANNING.INDEX_STATS_REQUIRED",
    },
    {
        "diagnostic_id": "optimizer.plan_cache.route_capability_mismatch",
        "area": "optimizer",
        "code": "SB_OPTIMIZER_PLAN_CACHE_ROUTE_CAPABILITY_MISMATCH",
        "message_key": "optimizer.plan_cache.route_capability_mismatch",
        "redaction_class": "metadata_only",
        "compatibility_status": "fail_closed_stable",
        "source_path": "src/engine/optimizer/optimizer_plan_cache.cpp",
        "source_token": "SB_OPTIMIZER_PLAN_CACHE_ROUTE_CAPABILITY_MISMATCH",
        "public_test_path": "tests/release/public_optimizer_expression_plan_cache_validation_gate.cpp",
        "public_test_token": "SB_OPTIMIZER_PLAN_CACHE_ROUTE_CAPABILITY_MISMATCH",
    },
    {
        "diagnostic_id": "agents.operator_approval.required",
        "area": "agents",
        "code": "SB_AGENT_APPROVAL.REQUIRED",
        "message_key": "agent.approval.required",
        "redaction_class": "metadata_only",
        "compatibility_status": "diagnostic_only_stable",
        "source_path": "src/core/agents/agent_runtime.cpp",
        "source_token": "SB_AGENT_APPROVAL.REQUIRED",
        "public_test_path": "tests/release/public_agent_operator_explain_cluster_boundary_gate.cpp",
        "public_test_token": "SB_AGENT_APPROVAL.REQUIRED",
    },
    {
        "diagnostic_id": "agents.cluster.external_provider_required",
        "area": "agents",
        "code": "CLUSTER.EXTERNAL_PROVIDER_REQUIRED",
        "message_key": "agent.cluster.external_provider_required",
        "redaction_class": "metadata_only",
        "compatibility_status": "unsupported_stable",
        "source_path": "src/core/agents/agent_cluster_boundary.hpp",
        "source_token": "CLUSTER.EXTERNAL_PROVIDER_REQUIRED",
        "public_test_path": "tests/release/public_agent_operator_explain_cluster_boundary_gate.cpp",
        "public_test_token": "external-provider-required diagnostic missing",
    },
    {
        "diagnostic_id": "indexes.temporary_work.runtime_missing",
        "area": "indexes",
        "code": "INDEX.TEMPORARY_WORK.RUNTIME_MISSING",
        "message_key": "index.temporary_work.runtime_missing",
        "redaction_class": "metadata_only",
        "compatibility_status": "fail_closed_stable",
        "source_path": "src/core/index/temporary_work_index_runtime.cpp",
        "source_token": "INDEX.TEMPORARY_WORK.RUNTIME_MISSING",
        "public_test_path": "tests/release/CMakeLists.txt",
        "public_test_token": "public_index_dml_maintenance_strategy_gate",
    },
    {
        "diagnostic_id": "indexes.bulk_publish.candidate_missing",
        "area": "indexes",
        "code": "SB-INDEX-BULK-PUBLISH-RECOVERY-CANDIDATE-MISSING",
        "message_key": "index.bulk_publish_recovery.candidate_missing",
        "redaction_class": "metadata_only",
        "compatibility_status": "fail_closed_stable",
        "source_path": "src/core/index/index_bulk_publish_recovery.cpp",
        "source_token": "SB-INDEX-BULK-PUBLISH-RECOVERY-CANDIDATE-MISSING",
        "public_test_path": "tests/fault_injection/public_crash_fault_matrix.py",
        "public_test_token": "index_update",
    },
    {
        "diagnostic_id": "cluster_boundary.support_not_enabled",
        "area": "cluster_boundary",
        "code": "SBLR.CLUSTER.SUPPORT_NOT_ENABLED",
        "message_key": "engine.cluster.support_not_enabled",
        "redaction_class": "metadata_only",
        "compatibility_status": "unsupported_stable",
        "source_path": "src/cluster_provider/cluster_provider.hpp",
        "source_token": "SBLR.CLUSTER.SUPPORT_NOT_ENABLED",
        "public_test_path": "tests/release/public_sblr_uuid_mga_route_integration_gate.cpp",
        "public_test_token": "SBLR.CLUSTER.SUPPORT_NOT_ENABLED",
    },
    {
        "diagnostic_id": "release_gate.platform.unsupported_first_release",
        "area": "release_gate",
        "code": "SB_DIAG_PLATFORM_UNSUPPORTED_FIRST_RELEASE",
        "message_key": "release.platform.unsupported_first_release",
        "redaction_class": "path_free",
        "compatibility_status": "unsupported_stable",
        "source_path": "tools/release/public_platform_matrix_gate.py",
        "source_token": "SB_DIAG_PLATFORM_UNSUPPORTED_FIRST_RELEASE",
        "public_test_path": "tests/release/CMakeLists.txt",
        "public_test_token": "public_platform_matrix_gate",
    },
    {
        "diagnostic_id": "release_gate.sanitizer.tsan_runtime_unavailable",
        "area": "release_gate",
        "code": "SB_DIAG_TSAN_RUNTIME_UNAVAILABLE",
        "message_key": "release.sanitizer.tsan_runtime_unavailable",
        "redaction_class": "path_free",
        "compatibility_status": "native_runner_required",
        "source_path": "tools/release/public_sanitizer_static_analysis_gate.py",
        "source_token": "SB_DIAG_TSAN_RUNTIME_UNAVAILABLE",
        "public_test_path": "tests/release/CMakeLists.txt",
        "public_test_token": "public_sanitizer_static_analysis_gate",
    },
    {
        "diagnostic_id": "release_gate.sanitizer.windows_asan_ubsan_runtime_unavailable",
        "area": "release_gate",
        "code": "SB_DIAG_WINDOWS_ASAN_UBSAN_RUNTIME_UNAVAILABLE",
        "message_key": "release.sanitizer.windows_asan_ubsan_runtime_unavailable",
        "redaction_class": "path_free",
        "compatibility_status": "native_runner_required",
        "source_path": "tools/release/public_sanitizer_static_analysis_gate.py",
        "source_token": "SB_DIAG_WINDOWS_ASAN_UBSAN_RUNTIME_UNAVAILABLE",
        "public_test_path": "tests/release/CMakeLists.txt",
        "public_test_token": "public_sanitizer_static_analysis_gate",
    },
    {
        "diagnostic_id": "diagnostics.support_bundle.authority_claim_refused",
        "area": "diagnostics",
        "code": "OPS.SUPPORT_BUNDLE.TRANSACTION_AUTHORITY_CLAIM_REFUSED",
        "message_key": "ops.support_bundle.transaction_authority_claim_refused",
        "redaction_class": "metadata_only",
        "compatibility_status": "diagnostic_only_stable",
        "source_path": "src/engine/internal_api/management/support_bundle_api.cpp",
        "source_token": "OPS.SUPPORT_BUNDLE.TRANSACTION_AUTHORITY_CLAIM_REFUSED",
        "public_test_path": "tests/release/public_transaction_support_bundle_gate.cpp",
        "public_test_token": "OPS.SUPPORT_BUNDLE.TRANSACTION_AUTHORITY_CLAIM_REFUSED",
    },
    {
        "diagnostic_id": "diagnostics.manager.lifecycle_state_write_failed",
        "area": "diagnostics",
        "code": "MANAGER.LIFECYCLE_STATE_WRITE_FAILED",
        "message_key": "manager.lifecycle.state_write_failed",
        "redaction_class": "path_free",
        "compatibility_status": "fail_closed_stable",
        "source_path": "src/manager/node/manager_lifecycle.cpp",
        "source_token": "MANAGER.LIFECYCLE_STATE_WRITE_FAILED",
        "public_test_path": "tests/manager/protocol_unit_tests.cpp",
        "public_test_token": "MANAGER.LIFECYCLE_STATE_WRITE_FAILED",
    },
    {
        "diagnostic_id": "diagnostics.manager.lifecycle_journal_write_failed",
        "area": "diagnostics",
        "code": "MANAGER.LIFECYCLE_JOURNAL_WRITE_FAILED",
        "message_key": "manager.lifecycle.journal_write_failed",
        "redaction_class": "path_free",
        "compatibility_status": "fail_closed_stable",
        "source_path": "src/manager/node/manager_lifecycle.cpp",
        "source_token": "MANAGER.LIFECYCLE_JOURNAL_WRITE_FAILED",
        "public_test_path": "tests/manager/protocol_unit_tests.cpp",
        "public_test_token": "MANAGER.LIFECYCLE_JOURNAL_WRITE_FAILED",
    },
    {
        "diagnostic_id": "diagnostics.manager.control_max_clients",
        "area": "diagnostics",
        "code": "MANAGER.CONTROL_MAX_CLIENTS",
        "message_key": "manager.control.max_clients",
        "redaction_class": "metadata_only",
        "compatibility_status": "fail_closed_stable",
        "source_path": "src/manager/node/manager_runtime.cpp",
        "source_token": "MANAGER.CONTROL_MAX_CLIENTS",
        "public_test_path": "tests/manager/runtime_integration_tests.cpp",
        "public_test_token": "MANAGER.CONTROL_MAX_CLIENTS",
    },
    {
        "diagnostic_id": "diagnostics.manager.audit_write_failed",
        "area": "diagnostics",
        "code": "MANAGER.AUDIT_WRITE_FAILED",
        "message_key": "manager.audit.write_failed",
        "redaction_class": "metadata_only",
        "compatibility_status": "fail_closed_stable",
        "source_path": "src/manager/node/manager_runtime.cpp",
        "source_token": "MANAGER.AUDIT_WRITE_FAILED",
        "public_test_path": "tests/manager/runtime_integration_tests.cpp",
        "public_test_token": "MANAGER.AUDIT_WRITE_FAILED",
    },
    {
        "diagnostic_id": "diagnostics.manager.release_profile_forbids_local_token_store",
        "area": "diagnostics",
        "code": "MANAGER.RELEASE_PROFILE_FORBIDS_LOCAL_TOKEN_STORE",
        "message_key": "manager.release_profile.forbids_local_token_store",
        "redaction_class": "metadata_only",
        "compatibility_status": "fail_closed_stable",
        "source_path": "src/manager/node/manager_runtime.cpp",
        "source_token": "MANAGER.RELEASE_PROFILE_FORBIDS_LOCAL_TOKEN_STORE",
        "public_test_path": "tests/manager/runtime_integration_tests.cpp",
        "public_test_token": "MANAGER.RELEASE_PROFILE_FORBIDS_LOCAL_TOKEN_STORE",
    },
    {
        "diagnostic_id": "diagnostics.manager.secret_file_unsafe",
        "area": "diagnostics",
        "code": "MANAGER.SECRET_FILE_UNSAFE",
        "message_key": "manager.secret.file_unsafe",
        "redaction_class": "redacted_detail",
        "compatibility_status": "fail_closed_stable",
        "source_path": "src/manager/node/manager_runtime.cpp",
        "source_token": "MANAGER.SECRET_FILE_UNSAFE",
        "public_test_path": "tests/manager/runtime_integration_tests.cpp",
        "public_test_token": "MANAGER.SECRET_FILE_UNSAFE",
    },
    {
        "diagnostic_id": "diagnostics.manager.dbbt_keyring_file_unsafe",
        "area": "diagnostics",
        "code": "MANAGER.DBBT_KEYRING_FILE_UNSAFE",
        "message_key": "manager.dbbt.keyring_file_unsafe",
        "redaction_class": "redacted_detail",
        "compatibility_status": "fail_closed_stable",
        "source_path": "src/manager/node/manager_runtime.cpp",
        "source_token": "MANAGER.DBBT_KEYRING_FILE_UNSAFE",
        "public_test_path": "tests/manager/runtime_integration_tests.cpp",
        "public_test_token": "MANAGER.DBBT_KEYRING_FILE_UNSAFE",
    },
)

CSV_FIELDS = (
    "diagnostic_id",
    "area",
    "code",
    "message_key",
    "redaction_class",
    "compatibility_status",
    "source_path",
    "public_test_path",
)


def fail(message: str) -> None:
    print(f"public_diagnostic_matrix_generator=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def reject_private_reference(value: str, context: str) -> None:
    if Path(value).is_absolute():
        fail(f"absolute_path_recorded:{context}:{value}")
    for fragment in FORBIDDEN_REFERENCE_FRAGMENTS:
        if fragment in value:
            fail(f"private_reference_recorded:{context}:{value}")


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def require_file(project_root: Path, relative: str) -> str:
    reject_private_reference(relative, "diagnostic_matrix_path")
    path = project_root / relative
    if not path.is_file():
        fail(f"missing_file:{relative}")
    return path.read_text(encoding="utf-8")


def require_token(project_root: Path, relative: str, token: str) -> dict[str, str]:
    reject_private_reference(token, f"diagnostic_matrix_token:{relative}")
    text = require_file(project_root, relative)
    if token not in text:
        fail(f"missing_token:{relative}:{token}")
    return {"path": relative, "sha256": sha256_text(text), "token": token}


def validate_rows(project_root: Path) -> list[dict[str, Any]]:
    seen_ids: set[str] = set()
    seen_codes: set[str] = set()
    seen_areas: set[str] = set()
    rows: list[dict[str, Any]] = []

    for row in MATRIX_ROWS:
        row_id = row["diagnostic_id"]
        code = row["code"]
        area = row["area"]
        if row_id in seen_ids:
            fail(f"duplicate_diagnostic_id:{row_id}")
        seen_ids.add(row_id)
        if code in seen_codes:
            fail(f"duplicate_diagnostic_code:{code}")
        seen_codes.add(code)
        seen_areas.add(area)

        for key in CSV_FIELDS:
            reject_private_reference(row[key], f"row:{row_id}:{key}")
            if not row[key]:
                fail(f"empty_field:{row_id}:{key}")
        if area not in REQUIRED_AREAS:
            fail(f"unknown_area:{row_id}:{area}")
        if row["redaction_class"] not in REDACTION_CLASSES:
            fail(f"unknown_redaction_class:{row_id}:{row['redaction_class']}")
        if row["compatibility_status"] not in COMPATIBILITY_STATUSES:
            fail(f"unknown_compatibility_status:{row_id}:{row['compatibility_status']}")
        if any(ch.isspace() for ch in code):
            fail(f"diagnostic_code_contains_whitespace:{row_id}")
        if "." not in row["message_key"]:
            fail(f"message_key_not_namespaced:{row_id}:{row['message_key']}")

        source = require_token(project_root, row["source_path"], row["source_token"])
        public_test = require_token(project_root, row["public_test_path"], row["public_test_token"])
        rows.append(
            {
                **{field: row[field] for field in CSV_FIELDS},
                "source_token": source["token"],
                "public_test_token": public_test["token"],
                "status": "stable_public_diagnostic",
            }
        )

    missing_areas = sorted(REQUIRED_AREAS - seen_areas)
    if missing_areas:
        fail("missing_required_areas:" + ",".join(missing_areas))
    return rows


def write_csv(path: Path, rows: list[dict[str, Any]]) -> str:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(CSV_FIELDS))
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row[field] for field in CSV_FIELDS})
    return path.read_text(encoding="utf-8")


def build_evidence(project_root: Path, matrix_output: Path) -> dict[str, Any]:
    if project_root.name != "project" or not project_root.is_dir():
        fail("project_root_must_be_project_directory")
    rows = validate_rows(project_root)
    matrix_text = write_csv(matrix_output, rows)
    area_counts: dict[str, int] = {}
    for row in rows:
        area_counts[row["area"]] = area_counts.get(row["area"], 0) + 1
    evidence: dict[str, Any] = {
        "schema_version": 1,
        "gate": "PCR-GATE-121",
        "marker": "PUBLIC_DIAGNOSTIC_MATRIX_GENERATOR",
        "policy": {
            "public_tree_inputs_only": True,
            "private_docs_required": False,
            "stable_codes_required": True,
            "redaction_class_required": True,
            "compatibility_status_required": True,
            "release_proof_is_evidence_only": True,
        },
        "matrix_path": matrix_output.name,
        "matrix_sha256": sha256_text(matrix_text),
        "row_count": len(rows),
        "area_counts": area_counts,
        "rows": rows,
    }
    evidence["evidence_sha256"] = sha256_text(
        json.dumps(evidence, sort_keys=True, separators=(",", ":"))
    )
    return evidence


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--matrix-output", type=Path, required=True)
    parser.add_argument("--evidence-output", type=Path, required=True)
    args = parser.parse_args()

    project_root = args.project_root.resolve()
    matrix_output = args.matrix_output.resolve()
    evidence_output = args.evidence_output.resolve()
    evidence = build_evidence(project_root, matrix_output)
    evidence_output.parent.mkdir(parents=True, exist_ok=True)
    evidence_output.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n",
                               encoding="utf-8")
    print(f"public_diagnostic_matrix_rows={evidence['row_count']}")
    print(f"public_diagnostic_matrix_sha256={evidence['matrix_sha256']}")
    print("public_diagnostic_stability_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
