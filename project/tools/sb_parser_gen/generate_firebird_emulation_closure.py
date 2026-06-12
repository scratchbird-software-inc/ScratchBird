#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate the SML-024 Firebird emulation/parser closure fixtures."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
from typing import Any


DEFAULT_OUTPUT_DIR = "project/tests/firebird_parser_worker/generated/emulation_closure"
MANIFEST_NAME = "SML_024_FIREBIRD_EMULATION_CLOSURE_MANIFEST.csv"
BOUNDARY_NAME = "SML_024_FIREBIRD_EMULATION_SOURCE_BOUNDARY.json"
SCHEMA_VERSION = "firebird.emulation_closure.sml_024.v1"

MANIFEST_COLUMNS = [
    "sml_id",
    "gate_id",
    "row_id",
    "proof_surface",
    "coverage_class",
    "source_authority",
    "ctest_resource_class",
    "executable_gate",
    "parser_role",
    "storage_authority",
    "finality_authority",
    "reference_engine_source_used",
    "reference_engine_storage_used",
    "reference_engine_finality_used",
    "reference_engine_sql_executed",
    "raw_upstream_payload_tracked",
    "network_required",
    "closure_status",
    "source_rename_risk",
    "evidence_paths",
    "evidence_tokens",
    "result_contract",
    "result_hash",
]


def sha256_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def canonical_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(",", ":"))


def row(
    *,
    row_id: str,
    proof_surface: str,
    coverage_class: str,
    source_authority: str,
    ctest_resource_class: str,
    executable_gate: str,
    parser_role: str,
    storage_authority: str,
    finality_authority: str,
    raw_upstream_payload_tracked: str,
    evidence_paths: str,
    evidence_tokens: str,
    result_contract: str,
    closure_status: str = "closed_proof",
    source_rename_risk: str = "none_detected",
) -> dict[str, str]:
    return {
        "sml_id": "SML-024",
        "gate_id": "SML-GATE-024",
        "row_id": row_id,
        "proof_surface": proof_surface,
        "coverage_class": coverage_class,
        "source_authority": source_authority,
        "ctest_resource_class": ctest_resource_class,
        "executable_gate": executable_gate,
        "parser_role": parser_role,
        "storage_authority": storage_authority,
        "finality_authority": finality_authority,
        "reference_engine_source_used": "false",
        "reference_engine_storage_used": "false",
        "reference_engine_finality_used": "false",
        "reference_engine_sql_executed": "false",
        "raw_upstream_payload_tracked": raw_upstream_payload_tracked,
        "network_required": "false",
        "closure_status": closure_status,
        "source_rename_risk": source_rename_risk,
        "evidence_paths": evidence_paths,
        "evidence_tokens": evidence_tokens,
        "result_contract": result_contract,
        "result_hash": sha256_text(result_contract),
    }


def manifest_rows() -> list[dict[str, str]]:
    return [
        row(
            row_id="SML-024-FIREBIRD-PARSER-PIPELINE-SOURCE",
            proof_surface="compatibility_parser_pipeline",
            coverage_class="scratchbird_owned_code",
            source_authority="scratchbird_owned_code",
            ctest_resource_class="local_ctest",
            executable_gate="firebird_parser_pipeline_probe",
            parser_role="parse_bind_lower_emit_sblr_only",
            storage_authority="scratchbird_engine_runtime",
            finality_authority="scratchbird_engine_runtime",
            raw_upstream_payload_tracked="false",
            evidence_paths=(
                "project/src/parsers/compatibility/firebird/firebird_dialect.cpp;"
                "project/src/parsers/compatibility/firebird/firebird_dialect.hpp;"
                "project/tests/firebird_parser_worker/firebird_parser_pipeline_probe.cpp"
            ),
            evidence_tokens=(
                "engine_authority=scratchbird;"
                "real_firebird_file_effects=false;"
                "reference_engine_sql_executed=false;"
                "sql_text_included=false"
            ),
            result_contract=(
                "Firebird parser inputs are reduced to ScratchBird-owned parser evidence "
                "and SBLR envelopes without reference engine execution."
            ),
        ),
        row(
            row_id="SML-024-FIREBIRD-LIFECYCLE-EMULATION",
            proof_surface="non_file_lifecycle_emulation",
            coverage_class="emulated_surface",
            source_authority="scratchbird_owned_code",
            ctest_resource_class="local_ctest",
            executable_gate="firebird_runtime_absence_gate",
            parser_role="emit_diagnostic_or_lifecycle_descriptor_only",
            storage_authority="scratchbird_engine_runtime",
            finality_authority="scratchbird_engine_runtime",
            raw_upstream_payload_tracked="false",
            evidence_paths=(
                "project/src/parsers/compatibility/firebird/firebird_dialect.cpp;"
                "project/tests/firebird_parser_worker/firebird_runtime_absence_probe.cpp"
            ),
            evidence_tokens=(
                "firebird_non_file_emulation_gate;"
                "core_continues_without_firebird_package;"
                "real_firebird_file_effects=false;"
                "reference_engine_sql_executed=false"
            ),
            result_contract=(
                "Database lifecycle and service-like compatibility surfaces are emulated "
                "as ScratchBird descriptors or diagnostics with no reference file effects."
            ),
        ),
        row(
            row_id="SML-024-FIREBIRD-WORKER-SESSION-BOUNDARY",
            proof_surface="worker_session_emulation",
            coverage_class="runtime_boundary",
            source_authority="scratchbird_owned_code",
            ctest_resource_class="local_ctest",
            executable_gate="firebird_worker_session_probe",
            parser_role="session_handle_emulation_only",
            storage_authority="scratchbird_engine_runtime",
            finality_authority="scratchbird_engine_runtime",
            raw_upstream_payload_tracked="false",
            evidence_paths=(
                "project/src/parsers/compatibility/firebird/firebird_worker_session.cpp;"
                "project/tests/firebird_parser_worker/firebird_worker_session_probe.cpp"
            ),
            evidence_tokens=(
                "reference_engine_sql_executed=false;"
                "parser_storage_authority=false;"
                "parser_transaction_finality_authority=false;"
                "runtime_policy=emulated_session_handle_admitted"
            ),
            result_contract=(
                "Worker-session handles remain ScratchBird-owned emulation state and do "
                "not become storage or transaction authority."
            ),
        ),
        row(
            row_id="SML-024-FIREBIRD-PARSER-SUPPORT-PACKAGE",
            proof_surface="parser_support_package",
            coverage_class="package_boundary",
            source_authority="scratchbird_owned_code",
            ctest_resource_class="local_ctest",
            executable_gate="sbu_firebird_parser_support_probe",
            parser_role="trusted_parser_support_bridge_only",
            storage_authority="scratchbird_engine_runtime",
            finality_authority="scratchbird_engine_runtime",
            raw_upstream_payload_tracked="false",
            evidence_paths=(
                "project/src/udr/sbu_firebird_parser_support/sbu_firebird_parser_support.cpp;"
                "project/tests/firebird_parser_worker/sbu_firebird_parser_support_probe.cpp"
            ),
            evidence_tokens=(
                "reference_storage_authority=false;"
                "reference_recovery_authority=false;"
                "real_firebird_file_effects=false"
            ),
            result_contract=(
                "The parser-support package shares ScratchBird-owned parser support code "
                "only and does not import reference storage or recovery authority."
            ),
        ),
        row(
            row_id="SML-024-FIREBIRD-REFERENCE-TEST-RESOURCE",
            proof_surface="external_regression_resource",
            coverage_class="external_ctest_resource",
            source_authority="scratchbird_harness_plus_external_resource",
            ctest_resource_class="external_ctest_resource",
            executable_gate="firebird_qa_replay_manifest_gate",
            parser_role="scratchbird_endpoint_replay_harness_only",
            storage_authority="scratchbird_engine_runtime",
            finality_authority="scratchbird_engine_runtime",
            raw_upstream_payload_tracked="false",
            evidence_paths=(
                "project/tests/reference_regression/reference_release_acquisition/firebird/5.0.4/regression/PUBLIC_REGRESSION_SCOPE.md;"
                "project/tests/reference_regression/reference_release_acquisition/firebird/5.0.4/regression/FIREBIRD_QA_REFERENCE_REPLAY_MANIFEST.csv;"
                "project/tests/firebird_parser_worker/firebird_qa_replay_manifest_gate.py"
            ),
            evidence_tokens=(
                "Reference source trees, source archives, and release-source evidence packets "
                "are intentionally not packaged in the public repository.;"
                "external_ctest_resource;"
                "firebird_reference_native"
            ),
            result_contract=(
                "Reference regression rows are CTest resources for replay and classification, "
                "not implementation source, storage, or finality authority."
            ),
        ),
        row(
            row_id="SML-024-FIREBIRD-REFERENCE-TOOL-SANDBOX",
            proof_surface="external_tool_resource",
            coverage_class="external_ctest_resource",
            source_authority="scratchbird_harness_plus_external_resource",
            ctest_resource_class="external_ctest_resource",
            executable_gate="firebird_reference_tool_sandbox_gate",
            parser_role="scratchbird_harness_only",
            storage_authority="scratchbird_engine_runtime",
            finality_authority="scratchbird_engine_runtime",
            raw_upstream_payload_tracked="false",
            evidence_paths=(
                "project/tests/firebird_parser_worker/firebird_reference_tool_gate.py;"
                "project/tests/firebird_parser_worker/fixtures/full_firebirdsql_parser_udr_emulation_closure/artifacts/REFERENCE_TOOL_SANDBOX_POLICY.md"
            ),
            evidence_tokens=(
                "loopback-only;"
                "isolated temp directories;"
                "strict timeouts;"
                "no external network"
            ),
            result_contract=(
                "Reference tools remain test-only resources isolated by CTest and are not "
                "linked into ScratchBird runtime products."
            ),
        ),
        row(
            row_id="SML-024-FIREBIRD-SOURCE-PROVENANCE-GATE",
            proof_surface="source_provenance_and_linkage",
            coverage_class="static_boundary",
            source_authority="scratchbird_owned_code",
            ctest_resource_class="local_ctest",
            executable_gate="firebird_clean_room_provenance_gate",
            parser_role="static_proof_gate_only",
            storage_authority="scratchbird_engine_runtime",
            finality_authority="scratchbird_engine_runtime",
            raw_upstream_payload_tracked="false",
            evidence_paths=(
                "project/tests/firebird_parser_worker/firebird_clean_room_provenance_gate.py;"
                "project/tests/firebird_parser_worker/firebird_runtime_isolation_gate.py;"
                "project/tests/firebird_parser_worker/generated/emulation_closure/firebird_sml_024_emulation_closure_gate.py"
            ),
            evidence_tokens=(
                "original ScratchBird code;"
                "Runtime link dependency checks;"
                "firebird_tool_runtime_isolation_gate;"
                "SML-GATE-024"
            ),
            result_contract=(
                "SML-024 closes on static source ownership, link isolation, and explicit "
                "false authority flags for reference source, storage, and finality."
            ),
        ),
    ]


def boundary_manifest() -> dict[str, Any]:
    rows = manifest_rows()
    return {
        "schema_version": SCHEMA_VERSION,
        "sml_id": "SML-024",
        "gate_id": "SML-GATE-024",
        "generated_artifacts": [MANIFEST_NAME, BOUNDARY_NAME],
        "implementation_source_roots": [
            {
                "path": "project/src/parsers/compatibility/firebird",
                "authority": "scratchbird_owned_code",
                "required_license": "MPL-2.0",
                "reference_engine_source_used": False,
            },
            {
                "path": "project/src/udr/sbu_firebird_parser_support",
                "authority": "scratchbird_owned_code",
                "required_license": "MPL-2.0",
                "reference_engine_source_used": False,
            },
        ],
        "external_ctest_resources": [
            {
                "path": (
                    "project/tests/reference_regression/reference_release_acquisition/"
                    "firebird/5.0.4/regression"
                ),
                "resource_class": "external_ctest_resource",
                "source_authority": "not_scratchbird_implementation_source",
                "raw_upstream_payload_tracked": False,
            },
            {
                "path": "project/tests/reference_regression/reference_catalog_seeds/firebird",
                "resource_class": "external_ctest_resource",
                "source_authority": "not_scratchbird_implementation_source",
                "raw_upstream_payload_tracked": False,
            },
        ],
        "authority_assertions": {
            "parser_executes_reference_engine_sql": False,
            "reference_engine_source_used": False,
            "reference_engine_storage_used": False,
            "reference_engine_finality_used": False,
            "network_required": False,
            "large_parser_source_renamed": False,
        },
        "required_ctest_gates": sorted({row["executable_gate"] for row in rows}),
        "manifest_sha256": sha256_text(canonical_json(rows)),
    }


def write_csv(path: Path, rows: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=MANIFEST_COLUMNS, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path("."))
    parser.add_argument("--output-dir", type=Path, default=Path(DEFAULT_OUTPUT_DIR))
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    output_dir = args.output_dir
    if not output_dir.is_absolute():
        output_dir = repo_root / output_dir

    rows = manifest_rows()
    write_csv(output_dir / MANIFEST_NAME, rows)
    write_json(output_dir / BOUNDARY_NAME, boundary_manifest())
    print(f"generated {MANIFEST_NAME} and {BOUNDARY_NAME} in {output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
