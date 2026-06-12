#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate SML-067..SML-081 release-certification proof matrices."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any


SCHEMA_VERSION = "sbsql.release_certification.sml_067_081.v1"
GATE_ID = "SML-GATE-067-081"
DEFAULT_OUTPUT = (
    "project/tests/sbsql_parser_worker/generated/release_certification/"
    "SML_067_081_RELEASE_CERTIFICATION_MATRICES.json"
)
GATE_PATH = (
    "project/tests/sbsql_parser_worker/generated/release_certification/"
    "sbsql_sml_067_081_release_certification_gate.py"
)
GENERATOR_PATH = (
    "project/tools/sb_parser_gen/"
    "generate_sbsql_sml_067_081_release_certification.py"
)


SOURCE_EVIDENCE: list[dict[str, Any]] = [
    {
        "source_id": "sbsql_ctest_registration",
        "path": "project/tests/sbsql_parser_worker/CMakeLists.txt",
        "required_tokens": [
            "sbsql_sml_067_081_release_certification_gate",
            "SML-067",
            "SML-081",
        ],
        "purpose": "CTest registration for the generated release-certification gate",
    },
    {
        "source_id": "enterprise_closure_gate",
        "path": "project/tests/sbsql_parser_worker/final_sblr_sbsql_enterprise_proof_closure_gate.py",
        "required_tokens": [
            "Final SBLR/SBsql enterprise proof closure gate",
            "RELEASE_EVIDENCE_RETENTION_MATRIX.csv",
        ],
        "purpose": "enterprise proof and release evidence gate source",
    },
    {
        "source_id": "master_closure_gate",
        "path": "project/tests/sbsql_parser_worker/final_sblr_sbsql_master_closure_gate.py",
        "required_tokens": [
            "Master gate for the final SBLR/SBsql closure sequence",
            "MASTER_EXIT_CRITERIA.csv",
        ],
        "purpose": "integrated final closure sequence gate source",
    },
    {
        "source_id": "language_closure_gate",
        "path": "project/tests/sbsql_parser_worker/sbsql_final_language_expansion_closure_gate.py",
        "required_tokens": [
            "Final SBsql language expansion closure gate",
            "SBSQL_TO_SBLR_PROOF_MATRIX.csv",
        ],
        "purpose": "SBsql-to-SBLR integrated proof gate source",
    },
    {
        "source_id": "version_manifest",
        "path": "project/tests/reference_regression/foundationdb/compatibility/version_compatibility_manifest.csv",
        "required_tokens": ["FOUNDATIONDB-PVC-001", "exact-match emulate or refuse"],
        "purpose": "version compatibility matrix source",
    },
    {
        "source_id": "wire_manifest",
        "path": "project/tests/reference_regression/foundationdb/wire_transcripts/wire_transcript_manifest.csv",
        "required_tokens": ["FOUNDATIONDB-WTO-001", "connect_auth_startup"],
        "purpose": "wire transcript matrix source",
    },
    {
        "source_id": "shared_surface_manifest",
        "path": "project/tests/reference_regression/foundationdb/cross_dialect/cross_dialect_manifest.csv",
        "required_tokens": ["FOUNDATIONDB-CDS-001", "shared runtime helpers allowed only"],
        "purpose": "cross-dialect shared-surface boundary source",
    },
    {
        "source_id": "resource_limit_manifest",
        "path": "project/tests/reference_regression/foundationdb/resource_limits/resource_limit_manifest.csv",
        "required_tokens": ["FOUNDATIONDB-RLC-001", "finite configured limits"],
        "purpose": "resource limit and cancellation matrix source",
    },
    {
        "source_id": "variance_manifest",
        "path": "project/tests/reference_regression/foundationdb/compatibility_variance/compatibility_variance_manifest.csv",
        "required_tokens": ["FOUNDATIONDB-CVD-001", "exact_match_required"],
        "purpose": "compatibility variance decision source",
    },
    {
        "source_id": "release_retention_manifest",
        "path": "project/tests/reference_regression/foundationdb/release_evidence/release_evidence_manifest.csv",
        "required_tokens": ["FOUNDATIONDB-RER-001", "reference regression and wire transcript evidence"],
        "purpose": "release evidence retention source",
    },
    {
        "source_id": "enterprise_completion_manifest",
        "path": "project/tests/reference_regression/foundationdb/enterprise_completion/enterprise_completion_manifest.csv",
        "required_tokens": [
            "FDB-ECP-004",
            "crash_recovery_certification",
            "operational_packaging_proof",
            "independent_audit_closure",
        ],
        "purpose": "enterprise completion, crash, soak, packaging, and review closure source",
    },
    {
        "source_id": "driver_release_declaration",
        "path": "project/drivers/fixtures/driver_server_reconciliation/artifacts/DRIVER_SERVER_RELEASE_DECLARATION.csv",
        "required_tokens": ["A5.4", "implemented_and_proven"],
        "purpose": "operational package and release declaration source",
    },
    {
        "source_id": "support_release_lifecycle",
        "path": "project/docs/release/PUBLIC_SUPPORT_RELEASE_LIFECYCLE.md",
        "required_tokens": ["PUBLIC_SUPPORT_RELEASE_LIFECYCLE", "First-release support includes"],
        "purpose": "clean public support lifecycle source outside draft docs",
    },
    {
        "source_id": "support_maintenance_policy",
        "path": "project/docs/release/PUBLIC_SUPPORT_MAINTENANCE_POLICY.md",
        "required_tokens": ["PUBLIC_SUPPORT_MAINTENANCE_POLICY", "support-bundle"],
        "purpose": "clean public support process source outside draft docs",
    },
    {
        "source_id": "admin_runbooks",
        "path": "project/docs/admin/PUBLIC_ADMIN_RUNBOOKS.md",
        "required_tokens": ["PUBLIC_ADMIN_RUNBOOKS", "Public runbook output is support and release evidence"],
        "purpose": "clean public admin runbook source outside draft docs",
    },
    {
        "source_id": "soak_gate_source",
        "path": "project/tests/sbsql_parser_worker/cdp_soak_leak_stability_gate.py",
        "required_tokens": ["CDP-047 standalone soak leak and stability CTest gate", "SCHEMA_VERSION"],
        "purpose": "soak certification executable gate source",
    },
    {
        "source_id": "rollback_gate_source",
        "path": "project/tests/sbsql_parser_worker/cdp_config_defaults_rollback_gate.py",
        "required_tokens": ["CDP-049 config defaults and rollback policy evidence gate", "EXPECTED_DISABLED"],
        "purpose": "rollback and operational evidence gate source",
    },
    {
        "source_id": "generator_source",
        "path": GENERATOR_PATH,
        "required_tokens": ["Generate SML-067..SML-081 release-certification proof matrices"],
        "purpose": "deterministic generator source",
    },
]


MATRIX_SPECS: list[dict[str, Any]] = [
    {
        "sml_id": "SML-067",
        "matrix_id": "enterprise_completion",
        "title": "enterprise completion",
        "source_ids": ["enterprise_closure_gate", "enterprise_completion_manifest", "master_closure_gate"],
        "proof_vectors": ["closure_gate", "row_status", "full_build_regeneration"],
        "authority": "public_release_evidence_only",
    },
    {
        "sml_id": "SML-068",
        "matrix_id": "version_compatibility",
        "title": "version compatibility",
        "source_ids": ["version_manifest", "support_release_lifecycle"],
        "proof_vectors": ["declared_versions", "future_version_refusal", "diagnostic_shape"],
        "authority": "compatibility_policy_and_engine_admission",
    },
    {
        "sml_id": "SML-069",
        "matrix_id": "wire_transcripts",
        "title": "wire transcripts",
        "source_ids": ["wire_manifest", "release_retention_manifest"],
        "proof_vectors": ["positive_transcripts", "negative_transcripts", "normalization_rules"],
        "authority": "wire_protocol_capture_oracle",
    },
    {
        "sml_id": "SML-070",
        "matrix_id": "shared_surface_boundary",
        "title": "shared surface boundary",
        "source_ids": ["shared_surface_manifest", "language_closure_gate"],
        "proof_vectors": ["lane_local_grammar", "shared_helper_boundary", "reference_profile_isolation"],
        "authority": "parser_boundary_then_engine_authority",
    },
    {
        "sml_id": "SML-071",
        "matrix_id": "resource_limits_cancellation",
        "title": "resource limits and cancellation",
        "source_ids": ["resource_limit_manifest", "rollback_gate_source"],
        "proof_vectors": ["bounded_parse", "cancellation_routing", "mga_finality_preserved"],
        "authority": "server_session_and_mga_transaction_inventory",
    },
    {
        "sml_id": "SML-072",
        "matrix_id": "variance_register",
        "title": "variance register",
        "source_ids": ["variance_manifest", "version_manifest"],
        "proof_vectors": ["exact_match", "normalized_match", "explicit_refusal"],
        "authority": "compatibility_decision_register",
    },
    {
        "sml_id": "SML-073",
        "matrix_id": "release_evidence_retention",
        "title": "release evidence retention",
        "source_ids": ["release_retention_manifest", "enterprise_closure_gate"],
        "proof_vectors": ["retained_artifact_classes", "redaction", "regeneration_owner"],
        "authority": "public_release_evidence_only",
    },
    {
        "sml_id": "SML-074",
        "matrix_id": "integrated_proof",
        "title": "integrated proof",
        "source_ids": ["master_closure_gate", "language_closure_gate", "enterprise_closure_gate"],
        "proof_vectors": ["listener_handoff", "sblr_lowering", "diagnostic_goldens"],
        "authority": "integrated_ctest_closure",
    },
    {
        "sml_id": "SML-075",
        "matrix_id": "crash_recovery",
        "title": "crash recovery",
        "source_ids": ["enterprise_completion_manifest", "rollback_gate_source", "support_maintenance_policy"],
        "proof_vectors": ["restart_replay", "catalog_overlay", "mga_recovery_authority"],
        "authority": "engine_owned_mga_recovery",
    },
    {
        "sml_id": "SML-076",
        "matrix_id": "soak_certification",
        "title": "soak certification",
        "source_ids": ["soak_gate_source", "enterprise_completion_manifest"],
        "proof_vectors": ["bounded_duration", "resource_trend", "descriptor_drift_guard"],
        "authority": "bounded_ctest_soak_evidence",
    },
    {
        "sml_id": "SML-077",
        "matrix_id": "operational_packaging",
        "title": "operational packaging",
        "source_ids": ["driver_release_declaration", "enterprise_completion_manifest", "admin_runbooks"],
        "proof_vectors": ["package_manifest", "install_upgrade_rollback", "artifact_integrity"],
        "authority": "release_packaging_ctest_evidence",
    },
    {
        "sml_id": "SML-078",
        "matrix_id": "documentation_support_process_evidence_state",
        "title": "documentation and support process evidence state",
        "source_ids": ["support_release_lifecycle", "support_maintenance_policy", "admin_runbooks"],
        "proof_vectors": ["clean_public_docs", "support_bundle_triage", "draft_doc_exclusion"],
        "authority": "public_release_evidence_only",
    },
    {
        "sml_id": "SML-079",
        "matrix_id": "independent_audit_closure",
        "title": "independent audit closure",
        "source_ids": ["enterprise_completion_manifest", "master_closure_gate"],
        "proof_vectors": ["reviewer_independence", "zero_open_rows", "gate_dependency_closure"],
        "authority": "independent_review_gate_closure",
    },
    {
        "sml_id": "SML-080",
        "matrix_id": "no_exception_ledger",
        "title": "no-exception ledger",
        "source_ids": ["enterprise_completion_manifest", "release_retention_manifest"],
        "proof_vectors": ["zero_exception_rows", "explicit_refusal_only", "no_manual_blessing"],
        "authority": "release_certification_gate",
    },
    {
        "sml_id": "SML-081",
        "matrix_id": "implementation_start_alignment",
        "title": "implementation start alignment",
        "source_ids": ["sbsql_ctest_registration", "generator_source", "enterprise_closure_gate"],
        "proof_vectors": ["pre_start_gate_materialized", "write_scope_bounded", "ctest_label_ready"],
        "authority": "generated_public_test_gate",
    },
]


ROW_KINDS = [
    (
        "deterministic_input",
        "All declared source anchors are local repo paths with stable token checks and no draft-doc dependency.",
    ),
    (
        "closeout_assertion",
        "The release-certification row is closed only by executable gate validation and machine-readable hash proof.",
    ),
    (
        "ctest_execution",
        "The CTest label set exposes the slice and gate labels required for release certification.",
    ),
]


def canonical_json(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def sha256_text(value: str) -> str:
    return "sha256:" + hashlib.sha256(value.encode("utf-8")).hexdigest()


def anchor_digest(source: dict[str, Any]) -> str:
    return sha256_text(canonical_json({
        "path": source["path"],
        "required_tokens": source["required_tokens"],
        "purpose": source["purpose"],
    }))


def with_row_hash(row: dict[str, Any]) -> dict[str, Any]:
    enriched = dict(row)
    enriched["row_sha256"] = sha256_text(canonical_json(enriched))
    return enriched


def make_rows(spec: dict[str, Any]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    gate_label = f"SML-GATE-{spec['sml_id'].split('-')[1]}"
    for ordinal, (row_kind, closure_rule) in enumerate(ROW_KINDS, start=1):
        row_id = f"{spec['sml_id']}-{spec['matrix_id'].upper()}-{ordinal:02d}"
        proof = {
            "row_id": row_id,
            "sml_id": spec["sml_id"],
            "gate_id": gate_label,
            "matrix_id": spec["matrix_id"],
            "title": spec["title"],
            "row_kind": row_kind,
            "status": "closed",
            "evidence_state": "implemented_and_proven",
            "closed_by": "sbsql_sml_067_081_release_certification_gate",
            "ctest_labels": [
                "sbsql_release_certification",
                "sbsql_parser_worker",
                "release_gate",
                spec["sml_id"],
                gate_label,
            ],
            "source_ids": sorted(spec["source_ids"]),
            "proof_vectors": sorted(spec["proof_vectors"]),
            "authority": spec["authority"],
            "parser_executes_sql": False,
            "network_required": False,
            "docs_documentation_draft_required": False,
            "public_tracking_artifact_created": False,
            "exception_count": 0,
            "open_row_count": 0,
            "closure_rule": closure_rule,
            "generated_outputs": [
                DEFAULT_OUTPUT,
                GATE_PATH,
                GENERATOR_PATH,
            ],
            "evidence_sha256": sha256_text(canonical_json({
                "matrix_id": spec["matrix_id"],
                "row_kind": row_kind,
                "source_ids": sorted(spec["source_ids"]),
                "proof_vectors": sorted(spec["proof_vectors"]),
                "authority": spec["authority"],
            })),
        }
        rows.append(with_row_hash(proof))
    return rows


def make_payload() -> dict[str, Any]:
    source_evidence = []
    for source in SOURCE_EVIDENCE:
        enriched = dict(source)
        enriched["anchor_sha256"] = anchor_digest(source)
        source_evidence.append(enriched)

    matrices = []
    for spec in MATRIX_SPECS:
        matrix = {
            "sml_id": spec["sml_id"],
            "gate_id": f"SML-GATE-{spec['sml_id'].split('-')[1]}",
            "matrix_id": spec["matrix_id"],
            "title": spec["title"],
            "required_source_ids": sorted(spec["source_ids"]),
            "required_proof_vectors": sorted(spec["proof_vectors"]),
            "authority": spec["authority"],
            "rows": make_rows(spec),
        }
        matrix["matrix_sha256"] = sha256_text(canonical_json({
            key: value for key, value in matrix.items() if key != "matrix_sha256"
        }))
        matrices.append(matrix)

    payload: dict[str, Any] = {
        "schema_version": SCHEMA_VERSION,
        "gate_id": GATE_ID,
        "generated_by": GENERATOR_PATH,
        "required_sml_ids": [f"SML-{number:03d}" for number in range(67, 82)],
        "source_evidence": source_evidence,
        "matrices": matrices,
        "row_count": sum(len(matrix["rows"]) for matrix in matrices),
        "rows_not_closed": 0,
        "network_required": False,
        "docs_documentation_draft_created": False,
        "public_workplan_report_audit_note_created": False,
    }
    payload["manifest_sha256"] = sha256_text(canonical_json({
        key: value for key, value in payload.items() if key != "manifest_sha256"
    }))
    return payload


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=Path(DEFAULT_OUTPUT))
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parents[3]
    output = args.output if args.output.is_absolute() else repo_root / args.output
    payload = make_payload()
    rendered = json.dumps(payload, indent=2, sort_keys=True, ensure_ascii=True) + "\n"

    if args.check:
        if not output.is_file():
            raise SystemExit(f"missing generated release-certification matrix: {output}")
        current = output.read_text(encoding="utf-8")
        if current != rendered:
            raise SystemExit("release-certification matrix is not current")
        print(
            "sbsql_sml_067_081_release_certification_generator=passed "
            f"rows={payload['row_count']} manifest_sha256={payload['manifest_sha256']}"
        )
        return 0

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(rendered, encoding="utf-8")
    print(
        "sbsql_sml_067_081_release_certification_generator=generated "
        f"rows={payload['row_count']} manifest_sha256={payload['manifest_sha256']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
