#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Audit SBSQL-MISS-017 long-soak/security/package/support evidence state."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any


RELEASE_FIXTURE = Path(
    "project/tests/sbsql_parser_worker/generated/release_certification/"
    "SML_067_081_RELEASE_CERTIFICATION_MATRICES.json"
)

PARSER_CMAKE = Path("project/tests/sbsql_parser_worker/CMakeLists.txt")
ENGINE_LISTENER_CMAKE = Path("project/tests/engine_listener_enterprise/CMakeLists.txt")

REQUIRED_CTESTS = {
    "sbsql_sml_067_081_release_certification_gate",
    "cdp_config_defaults_rollback_gate",
    "cdp_soak_leak_stability_gate",
    "engine_listener_support_bundle_redaction_gate",
    "sbsql_long_soak_security_packaging_support_audit_gate",
}

REQUIRED_MATRIX_IDS = {
    "enterprise_completion",
    "version_compatibility",
    "wire_transcripts",
    "shared_surface_boundary",
    "resource_limits_cancellation",
    "variance_register",
    "release_evidence_retention",
    "integrated_proof",
    "crash_recovery",
    "soak_certification",
    "operational_packaging",
    "documentation_support_process_evidence_state",
    "independent_audit_closure",
    "no_exception_ledger",
    "implementation_start_alignment",
}

SBSQL_MISS_017_MATRIX_IDS = {
    "soak_certification",
    "operational_packaging",
    "documentation_support_process_evidence_state",
    "independent_audit_closure",
    "no_exception_ledger",
}

SOURCE_TOKENS = {
    Path("project/tests/sbsql_parser_worker/cdp_soak_leak_stability_gate.py"): (
        "CDP-047 standalone soak leak and stability CTest gate",
        "GROWTH_BUDGETS",
        "ROUTE_NAMES = (\"embedded\", \"local-ipc\", \"inet\")",
        "PUBLIC_TEST_PASSWORD, seed_database",
        "seed_database(",
        "parser_finality_authority",
        "reference_finality_authority",
        "mga_relation_metadata=",
    ),
    Path("project/tests/sbsql_parser_worker/cdp_config_defaults_rollback_gate.py"): (
        "CDP-049 config defaults and rollback policy evidence gate",
        "candidate_manifest",
        "DML.NATIVE_BULK_INGEST.DISABLED",
        "PUBLIC_TEST_PASSWORD, seed_database",
        "seed_database(",
        "parser_finality_authority",
        "reference_finality_authority",
    ),
    Path("project/tests/engine_listener_enterprise/engine_listener_support_bundle_redaction_gate.cpp"): (
        "support_bundle_redaction_bypass_refusal",
        "secret-canary-eler088-memory",
        "support_bundle_policy_installed:false",
        "<protected-material-excluded>",
        "engine_listener_support_bundle_redaction_gate=passed",
    ),
    Path("project/tools/sb_parser_gen/generate_sbsql_sml_067_081_release_certification.py"): (
        "operational_packaging_proof",
        "independent_audit_closure",
        "support_maintenance_policy",
        "admin_runbooks",
    ),
}

PUBLIC_DOC_TOKENS = {
    Path("project/docs/release/PUBLIC_SUPPORT_RELEASE_LIFECYCLE.md"): (
        "PUBLIC_SUPPORT_RELEASE_LIFECYCLE",
        "First-release support includes",
        "public_release_evidence_only",
    ),
    Path("project/docs/release/PUBLIC_SUPPORT_MAINTENANCE_POLICY.md"): (
        "PUBLIC_SUPPORT_MAINTENANCE_POLICY",
        "support-bundle",
        "Support evidence cannot replace authorization",
    ),
    Path("project/docs/admin/PUBLIC_ADMIN_RUNBOOKS.md"): (
        "PUBLIC_ADMIN_RUNBOOKS",
        "Public runbook output is support and release evidence",
        "The engine executes SBLR and internal procedures only",
    ),
}


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def require(condition: bool, message: str, errors: list[str]) -> None:
    if not condition:
        errors.append(message)


def safe_public_path(path: Path) -> bool:
    normalized = path.as_posix()
    return (
        not path.is_absolute()
        and ".." not in path.parts
        and "docs/documentation/draft" not in normalized
    )


def validate_cmake(repo: Path, errors: list[str]) -> None:
    parser_cmake = read_text(repo / PARSER_CMAKE)
    listener_cmake = read_text(repo / ENGINE_LISTENER_CMAKE)
    combined = parser_cmake + "\n" + listener_cmake
    for test_name in sorted(REQUIRED_CTESTS):
        require(test_name in combined, f"CTest registration missing: {test_name}", errors)
    require("SBSQL-MISS-GATE-017" in parser_cmake, "CMake missing SBSQL-MISS-GATE-017 label", errors)
    require(
        "sbsql_missing_functionality_implementation_closure" in parser_cmake,
        "CMake missing SBSQL missing-functionality closure label",
        errors,
    )
    require("ELER-088" in listener_cmake, "support-bundle redaction CMake label missing", errors)


def validate_source_tokens(repo: Path, errors: list[str]) -> None:
    for rel, tokens in {**SOURCE_TOKENS, **PUBLIC_DOC_TOKENS}.items():
        require(safe_public_path(rel), f"unsafe public proof path: {rel}", errors)
        path = repo / rel
        if not path.is_file():
            errors.append(f"proof source missing: {rel}")
            continue
        text = read_text(path)
        for token in tokens:
            require(token in text, f"{rel}: missing token {token!r}", errors)


def validate_release_fixture(repo: Path, errors: list[str]) -> tuple[int, int]:
    fixture = repo / RELEASE_FIXTURE
    require(fixture.is_file(), f"release fixture missing: {RELEASE_FIXTURE}", errors)
    if not fixture.is_file():
        return 0, 0
    payload = json.loads(read_text(fixture))
    require(
        payload.get("schema_version") == "sbsql.release_certification_state.sml_067_081.v2",
        "release fixture schema drift",
        errors,
    )
    require(payload.get("row_count") == 45, "release fixture row_count drift", errors)
    require(payload.get("rows_not_closed") == 45, "release fixture open-row count drift", errors)
    require(payload.get("release_certified") is False, "release fixture false finality drift", errors)
    require(payload.get("workplan_status") == "in_progress", "release workplan status drift", errors)
    require(
        payload.get("open_acceptance_phases") == ["IA-14", "IA-15"],
        "release open-acceptance phases drift",
        errors,
    )
    require(payload.get("network_required") is False, "release fixture network dependency drift", errors)
    require(
        payload.get("docs_documentation_draft_created") is False,
        "release fixture draft-doc dependency drift",
        errors,
    )
    require(
        payload.get("public_workplan_report_audit_note_created") is False,
        "release fixture public tracking artifact drift",
        errors,
    )
    matrices = payload.get("matrices", [])
    require(isinstance(matrices, list) and len(matrices) == 15, "release matrix count drift", errors)
    matrix_ids = {str(matrix.get("matrix_id", "")) for matrix in matrices}
    require(matrix_ids == REQUIRED_MATRIX_IDS, f"release matrix id drift: {sorted(matrix_ids)}", errors)
    rows_checked = 0
    for matrix in matrices:
        matrix_id = str(matrix.get("matrix_id", ""))
        rows = matrix.get("rows", [])
        if matrix_id in SBSQL_MISS_017_MATRIX_IDS:
            require(len(rows) == 3, f"{matrix_id} row count drift", errors)
            for row in rows:
                rows_checked += 1
                row_id = str(row.get("row_id", ""))
                require(row.get("status") == "acceptance_open", f"{row_id} acceptance state drift", errors)
                require(
                    row.get("evidence_state") == "structural_and_declarative_evidence_retained",
                    f"{row_id} evidence state drift",
                    errors,
                )
                require(
                    row.get("closed_by") == "not_applicable_release_acceptance_open",
                    f"{row_id} false closure identity drift",
                    errors,
                )
                require(row.get("parser_executes_sql") is False, f"{row_id} parser authority drift", errors)
                require(row.get("network_required") is False, f"{row_id} network dependency drift", errors)
                require(
                    row.get("docs_documentation_draft_required") is False,
                    f"{row_id} draft-doc dependency drift",
                    errors,
                )
                require(
                    row.get("public_tracking_artifact_created") is False,
                    f"{row_id} public tracking artifact drift",
                    errors,
                )
                require(row.get("exception_count") == 0, f"{row_id} exception count drift", errors)
                require(row.get("open_row_count") == 1, f"{row_id} open row count drift", errors)
                for output in row.get("generated_outputs", []):
                    require(safe_public_path(Path(str(output))), f"{row_id} unsafe output path {output}", errors)
    return len(matrices), rows_checked


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()

    repo = Path(args.repo_root).resolve()
    errors: list[str] = []
    try:
        validate_cmake(repo, errors)
        validate_source_tokens(repo, errors)
        matrix_count, rows_checked = validate_release_fixture(repo, errors)
    except Exception as exc:  # noqa: BLE001 - CTest should get the concrete failure.
        print("sbsql_long_soak_security_packaging_support_audit_gate=failed", file=sys.stderr)
        print(str(exc), file=sys.stderr)
        return 2

    if errors:
        print("sbsql_long_soak_security_packaging_support_audit_gate=failed", file=sys.stderr)
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    print("sbsql_long_soak_security_packaging_support_audit_gate=passed")
    print(f"required_ctests={len(REQUIRED_CTESTS)}")
    print(f"release_matrices={matrix_count}")
    print(f"sbsql_miss_017_rows_checked={rows_checked}")
    print("long_soak_security_packaging_support_audit=evidence_retained_acceptance_open")
    print("release_certified=false")
    print("open_acceptance_phases=IA-14,IA-15")
    print("engine_authority=SBLR_UUID_MGA_only")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
