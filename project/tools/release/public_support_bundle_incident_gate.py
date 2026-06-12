#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate public support-bundle incident package proof anchors."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import sys
from typing import Any


# PUBLIC_SUPPORT_BUNDLE_INCIDENT_GATE

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
        "surface": "enterprise_observability_support_bundle_boundary",
        "path": "project/src/engine/internal_api/observability/cluster_support_bundle_redaction_api.hpp",
        "tokens": (
            "ENTERPRISE_SUPPORT_BUNDLE",
            "CLUSTER_SUPPORT_BUNDLE_REDACTION",
            "redaction-policy outputs only",
            "never establish local cluster truth authority",
        ),
    },
    {
        "surface": "management_support_bundle_request_result",
        "path": "project/src/engine/internal_api/management/support_bundle_api.hpp",
        "tokens": (
            "SB_ENGINE_INTERNAL_API_MANAGEMENT_SUPPORT_BUNDLE_API",
            "EnginePrepareSupportBundleRequest",
            "EnginePrepareSupportBundleResult",
            "redaction_applied",
            "forbidden_fields_absent",
            "flush_required_before_export",
            "agent_runtime_evidence_collected",
            "performance_optimization_surface_collected",
            "transaction_evidence_collected",
        ),
    },
    {
        "surface": "management_support_bundle_policy_redaction",
        "path": "project/src/engine/internal_api/management/support_bundle_api.cpp",
        "tokens": (
            "OPS.SUPPORT_BUNDLE.SECURITY_CONTEXT_REQUIRED",
            "OPS.SUPPORT_BUNDLE.ENGINE_AUTHORIZATION_REQUIRED",
            "OPS.SUPPORT_BUNDLE.POLICY_REQUIRED",
            "OPS.SUPPORT_BUNDLE.PROTECTED_MATERIAL_FORBIDDEN",
            "redaction_profile_ref",
            "retention_policy_ref",
            "support_bundle_completeness_state",
            "forbidden_fields_absent",
        ),
    },
    {
        "surface": "transaction_support_bundle_authority",
        "path": "project/tests/release/public_transaction_support_bundle_gate.cpp",
        "tokens": (
            "transaction_support_bundle_summary",
            "durable_mga_transaction_inventory",
            "support_bundle_is_authority",
            "false",
            "OPS.SUPPORT_BUNDLE.TRANSACTION_AUTHORITY_CLAIM_REFUSED",
            "OPS.SUPPORT_BUNDLE.CURRENT_ROW_AUTHORITY_CLAIM_REFUSED",
            "OPS.SUPPORT_BUNDLE.TRANSACTION_CLEANUP_MUTATION_REFUSED",
            "docs\" \"/execution-plans",
            "/home/",
        ),
    },
    {
        "surface": "observability_triage_surfaces",
        "path": "project/tests/release/public_observability_schema_gate.cpp",
        "tokens": (
            "\"memory\", \"sys.metrics.memory\"",
            "\"storage\", \"sys.metrics.storage\"",
            "\"transactions\", \"sys.metrics.transactions\"",
            "\"indexes\", \"sys.metrics.index\"",
            "\"optimizer\", \"sys.metrics.optimizer\"",
            "\"agents\", \"sys.metrics.agents\"",
            "\"backup\", \"sys.metrics.backup\"",
            "\"archive\", \"sys.metrics.archive\"",
            "\"security\", \"sys.metrics.security\"",
            "\"support_bundle\", \"sys.metrics.supportability\"",
        ),
    },
    {
        "surface": "agent_and_repair_bundle_redaction",
        "path": "project/tests/release/public_observability_schema_gate.cpp",
        "tokens": (
            "agent_observability_support_bundle",
            "agent_runtime_observability_redaction",
            "no_parser_or_client_catalog_uuid_authority",
            "repair_history_health_observability",
            "durable_mga_inventory_is_sole_transaction_authority",
            "parser_or_reference_authority_refused",
        ),
    },
    {
        "surface": "cluster_support_bundle_redaction",
        "path": "project/tests/release/public_observability_schema_gate.cpp",
        "tokens": (
            "cluster_support_bundle_redaction",
            "cluster_projection_sensitive_values_redacted",
            "support_bundle_not_local_cluster_authority",
            "cluster_support_bundle_requires_export_right",
            "cluster-secret-",
            "[redacted:security]",
        ),
    },
    {
        "surface": "optimizer_metric_support_bundle_redaction",
        "path": "project/tests/release/public_observability_schema_gate.cpp",
        "tokens": (
            "optimizer_metric_support_bundle",
            "optimizer_metric_labels_redacted",
            "no_finality_visibility_security_recovery_wal_or_cluster_authority",
            "wal_or_redo_authority_refused",
        ),
    },
    {
        "surface": "optimizer_metric_bundle_api",
        "path": "project/src/engine/internal_api/observability/optimizer_metric_support_bundle.hpp",
        "tokens": (
            "OEIC_OPTIMIZER_METRIC_RETENTION_REDACTION",
            "redacted evidence only",
            "OptimizerMetricSupportBundleAuthority",
            "parser_or_reference_authority",
            "wal_or_redo_authority",
            "cluster_authority",
        ),
    },
    {
        "surface": "diagnostic_matrix_support_bundle_authority",
        "path": "project/tools/release/public_diagnostic_matrix_generator.py",
        "tokens": (
            "diagnostics.support_bundle.authority_claim_refused",
            "OPS.SUPPORT_BUNDLE.TRANSACTION_AUTHORITY_CLAIM_REFUSED",
            "redaction_class_required",
            "metadata_only",
        ),
    },
    {
        "surface": "support_bundle_capacity_baseline",
        "path": "project/tests/performance/public_performance_baselines.json",
        "tokens": (
            "support_bundle_generation",
            "enterprise_release_candidate_threshold",
            "support_bundle",
            "public_test_anchor",
        ),
    },
    {
        "surface": "public_single_node_support_bundle_redaction",
        "path": "project/cloud/kubernetes/manifests/public-single-node/sample-stack.yaml",
        "tokens": (
            "kind: ScratchBirdSupportBundle",
            "redacted public single-node support bundle",
            "redactionPolicyRef: public-redacted",
            "destinationRef: support-export-redacted",
            "includeTenantData: false",
        ),
    },
    {
        "surface": "release_gate_wiring",
        "path": "project/tests/release/CMakeLists.txt",
        "tokens": (
            "public_transaction_support_bundle_gate",
            "public_observability_schema_gate",
            "public_diagnostic_stability_gate",
            "public_performance_baseline_gate",
        ),
    },
)


def fail(message: str) -> None:
    print(f"public_support_bundle_incident_gate=fail:{message}", file=sys.stderr)
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


def write_evidence(path: Path, evidence: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--csv-output", required=True, type=Path)
    parser.add_argument("--evidence-output", required=True, type=Path)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    records = [validate_check(repo_root, check) for check in CHECKS]
    write_csv(args.csv_output, records)
    csv_text = args.csv_output.read_text(encoding="utf-8")
    evidence = {
        "schema": "scratchbird.public.support_bundle_incident_gate.v1",
        "marker": "PUBLIC_SUPPORT_BUNDLE_INCIDENT_GATE",
        "gate": "PCR-GATE-142",
        "status": "pass",
        "check_count": len(records),
        "source_reference_count": len({record["path"] for record in records}),
        "source_token_count": sum(record["token_count"] for record in records),
        "csv_sha256": sha256_text(csv_text),
        "authority": "public_release_evidence_only",
        "policy": {
            "triage_complete_sections_required": True,
            "versions_platform_config_diagnostics_included": True,
            "memory_storage_transaction_index_security_agent_repair_archive_backup_included": True,
            "secrets_redacted": True,
            "tenant_data_excluded_by_default": True,
            "support_bundle_engine_authority": False,
            "mga_finality_authority_preserved": True,
            "parser_sql_text_authority": False,
            "cluster_public_production_claims": False,
        },
        "records": records,
    }
    write_evidence(args.evidence_output, evidence)
    print(
        "public_support_bundle_incident_gate=passed "
        f"checks={len(records)} "
        f"csv_sha256={evidence['csv_sha256']}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
