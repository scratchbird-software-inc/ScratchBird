#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate enterprise engine/listener documentation coverage."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import sys
from typing import Any


# PUBLIC_ENTERPRISE_DOCUMENTATION_GATE
# ENGINE_LISTENER_ENTERPRISE_DOCS

DOC_PATH = Path("project/docs/engine_listener/ENTERPRISE_RELEASE_GUIDE.md")
ADMIN_RUNBOOK_PATH = Path("project/docs/admin/PUBLIC_ADMIN_RUNBOOKS.md")
SUPPORT_LIFECYCLE_PATH = Path("project/docs/release/PUBLIC_SUPPORT_RELEASE_LIFECYCLE.md")
PUBLIC_API_POLICY_PATH = Path("project/docs/public_api/CORE_BETA_PUBLIC_COMPATIBILITY_POLICY.md")
RELEASE_CMAKE = Path("project/tests/release/CMakeLists.txt")
ENTERPRISE_CMAKE = Path("project/tests/engine_listener_enterprise/CMakeLists.txt")
DATABASE_LIFECYCLE_CMAKE = Path("project/tests/database_lifecycle/CMakeLists.txt")
FAULT_INJECTION_CMAKE = Path("project/tests/fault_injection/CMakeLists.txt")
OPTIMIZER_CMAKE = Path("project/tests/optimizer/CMakeLists.txt")

FORBIDDEN_REFERENCE_FRAGMENTS = (
    "docs" + "/" + "execution-plans",
    "docs" + "/" + "completed-execution-plans",
    "docs" + "/" + "findings",
    "docs" + "/" + "audit",
    "." + "git",
    "/" + "home" + "/",
    "ScratchBird" + "-Private",
    "local" + "_work",
)

STATUS_TERMS = (
    "production-supported",
    "provider-required",
    "cluster-provider-only",
    "disabled-by-default",
    "diagnostic-only",
    "evidence-only",
    "experimental",
    "unsupported",
)

POLICY_TOKENS = (
    "ENGINE_LISTENER_ENTERPRISE_DOCS",
    "public_release_evidence_only",
    "The engine executes admitted SBLR and internal APIs only",
    "Parser output is translation evidence",
    "MGA transaction inventory remains transaction finality authority",
    "Durable authorization state remains authorization authority",
    "Indexes and optimizer evidence are never final row authority",
    "Cluster-positive execution is outside the public core",
)

DOC_ROWS: tuple[dict[str, Any], ...] = (
    {
        "section": "DOC_ARCHITECTURE_OVERVIEW",
        "topic": "architecture",
        "doc_tokens": ("listener", "parser-worker boundary", "engine execution route"),
        "evidence": (
            (ENTERPRISE_CMAKE, ("engine_listener_integrated_product_proof_gate", "engine_listener_operational_readiness_gate")),
            (RELEASE_CMAKE, ("public_api_boundary_gate",)),
        ),
    },
    {
        "section": "DOC_TRUST_BOUNDARY_MODEL",
        "topic": "trust_boundary",
        "doc_tokens": ("Untrusted inputs include", "Trusted authority is intentionally narrow"),
        "evidence": (
            (ENTERPRISE_CMAKE, ("engine_listener_adversarial_security_validation_gate",)),
            (RELEASE_CMAKE, ("public_enterprise_threat_gate", "public_cluster_provider_boundary_cleanup_gate")),
        ),
    },
    {
        "section": "DOC_PARSER_LISTENER_ENGINE_BOUNDARY",
        "topic": "parser_listener_engine_boundary",
        "doc_tokens": ("Parser workers can translate", "SBSQL parser contracts must stay SBSQL-native"),
        "evidence": (
            (ENTERPRISE_CMAKE, ("engine_listener_dbbt_lpreface_binding_conformance", "engine_listener_parser_contract_freeze_gate", "engine_listener_sbsql_parser_sync_gate")),
        ),
    },
    {
        "section": "DOC_STARTUP_OPEN_LIFECYCLE",
        "topic": "startup_open_lifecycle",
        "doc_tokens": ("Startup validates configuration", "Database open validates headers"),
        "evidence": (
            (RELEASE_CMAKE, ("public_default_config_check", "public_secure_defaults_gate")),
            (DATABASE_LIFECYCLE_CMAKE, ("database_lifecycle_shutdown_conformance",)),
        ),
    },
    {
        "section": "DOC_FILE_FORMAT_OVERVIEW",
        "topic": "file_format",
        "doc_tokens": ("All durable metadata has explicit version", "Unsupported old formats"),
        "evidence": (
            (ENTERPRISE_CMAKE, ("engine_listener_compatibility_upgrade_downgrade_gate",)),
            (RELEASE_CMAKE, ("public_release_version_metadata_gate", "public_upgrade_migration_gate")),
        ),
    },
    {
        "section": "DOC_TRANSACTION_MGA_MODEL",
        "topic": "transaction_mga",
        "doc_tokens": ("MGA/COW versioning", "Page finality is not transaction finality"),
        "evidence": (
            (RELEASE_CMAKE, ("public_transaction_mga_cow_gate",)),
            (FAULT_INJECTION_CMAKE, ("transaction_inventory_publish_fault_conformance",)),
            (ENTERPRISE_CMAKE, ("engine_listener_mga_integrated_physical_cleanup_conformance",)),
        ),
    },
    {
        "section": "DOC_ISOLATION_SEMANTICS",
        "topic": "isolation",
        "doc_tokens": ("snapshot visibility", "uncertain recovery or visibility state fails closed"),
        "evidence": (
            (ENTERPRISE_CMAKE, ("engine_listener_serializable_isolation_conformance", "engine_listener_crash_recovery_certification_gate", "engine_listener_fuzz_property_invariant_suite_gate")),
        ),
    },
    {
        "section": "DOC_CRASH_RECOVERY_MODEL",
        "topic": "crash_recovery",
        "doc_tokens": ("Recovery is certified", "Silent inconsistency is not an accepted outcome"),
        "evidence": (
            (ENTERPRISE_CMAKE, ("engine_listener_crash_fault_campaign_gate", "engine_listener_crash_recovery_certification_gate")),
            (RELEASE_CMAKE, ("public_crash_fault_gate",)),
        ),
    },
    {
        "section": "DOC_SECURITY_MODEL",
        "topic": "security",
        "doc_tokens": ("Explicit deny overrides allow", "support-bundle redaction"),
        "evidence": (
            (ENTERPRISE_CMAKE, ("engine_listener_materialized_authorization_conformance", "engine_listener_tls_channel_binding_conformance", "engine_listener_support_bundle_redaction_gate")),
        ),
    },
    {
        "section": "DOC_AUTH_PROVIDER_MODEL",
        "topic": "auth_provider",
        "doc_tokens": ("Auth providers are admitted", "Provider evidence does not become engine authority"),
        "evidence": (
            (RELEASE_CMAKE, ("public_security_provider_contract_protected_material_gate", "public_authorization_durable_flow_gate", "public_enterprise_threat_gate")),
        ),
    },
    {
        "section": "DOC_AUTHORIZATION_MODEL",
        "topic": "authorization",
        "doc_tokens": ("Authorization is evaluated from durable principals", "Parser routes cannot promote"),
        "evidence": (
            (DATABASE_LIFECYCLE_CMAKE, ("dpc_security_privilege_gate", "database_lifecycle_config_policy_security_provider_conformance")),
            (ENTERPRISE_CMAKE, ("engine_listener_management_envelope_conformance",)),
        ),
    },
    {
        "section": "DOC_PAGE_INDEX_TOAST_MODEL",
        "topic": "page_index_toast",
        "doc_tokens": ("Index candidates can accelerate lookup", "TOAST cleanup must preserve values reachable"),
        "evidence": (
            (RELEASE_CMAKE, ("public_page_body_checksum_agreement_gate", "public_toast_overflow_binary_descriptor_gate")),
            (ENTERPRISE_CMAKE, ("engine_listener_index_family_dml_route_conformance",)),
        ),
    },
    {
        "section": "DOC_OPTIMIZER_MODEL",
        "topic": "optimizer",
        "doc_tokens": ("Optimizer plans are evidence", "never override authorization"),
        "evidence": (
            (ENTERPRISE_CMAKE, ("engine_listener_optimizer_integrated_route_conformance",)),
            (RELEASE_CMAKE, ("public_optimizer_catalog_backed_planning_gate",)),
            (OPTIMIZER_CMAKE, ("optimizer_enterprise_plan_cache_gate",)),
        ),
    },
    {
        "section": "DOC_MEMORY_GOVERNANCE_MODEL",
        "topic": "memory",
        "doc_tokens": ("Memory governance uses explicit default manager", "Memory-pressure diagnostics are evidence-only"),
        "evidence": (
            (ENTERPRISE_CMAKE, ("engine_listener_memory_integrated_conformance",)),
            (RELEASE_CMAKE, ("public_query_memory_reservation_gate", "public_memory_pressure_executor_gate")),
        ),
    },
    {
        "section": "DOC_CONFIGURATION_REFERENCE",
        "topic": "configuration",
        "doc_tokens": ("Unsupported or unsafe configurations fail closed", "Cluster-positive routes are provider-required"),
        "evidence": (
            (ENTERPRISE_CMAKE, ("engine_listener_release_profile_completeness_gate",)),
            (RELEASE_CMAKE, ("public_default_config_check", "public_secure_defaults_gate")),
        ),
    },
    {
        "section": "DOC_OPERATIONAL_RUNBOOK",
        "topic": "operational_runbook",
        "doc_tokens": ("project/docs/admin/PUBLIC_ADMIN_RUNBOOKS.md", "Runbook output is evidence-only"),
        "evidence": (
            (ADMIN_RUNBOOK_PATH, ("PUBLIC_ADMIN_RUNBOOKS", "RUNBOOK_CREATE_DATABASE", "RUNBOOK_UPGRADE")),
            (RELEASE_CMAKE, ("public_admin_runbook_gate", "public_diagnostic_stability_gate")),
        ),
    },
    {
        "section": "DOC_TROUBLESHOOTING_GUIDE",
        "topic": "troubleshooting",
        "doc_tokens": ("Troubleshooting starts with stable diagnostics", "recovery-required diagnostics"),
        "evidence": (
            (ENTERPRISE_CMAKE, ("engine_listener_operational_readiness_gate",)),
            (DATABASE_LIFECYCLE_CMAKE, ("database_lifecycle_observability_conformance", "dpc_management_observability_support_bundle_gate")),
        ),
    },
    {
        "section": "DOC_SUPPORT_BUNDLE_GUIDE",
        "topic": "support_bundle",
        "doc_tokens": ("Support bundles are redacted by default", "must not leak secrets"),
        "evidence": (
            (ENTERPRISE_CMAKE, ("engine_listener_support_bundle_redaction_gate", "engine_listener_support_bundle_conformance")),
            (RELEASE_CMAKE, ("public_crypto_entropy_policy_gate",)),
        ),
    },
    {
        "section": "DOC_UPGRADE_GUIDE",
        "topic": "upgrade",
        "doc_tokens": ("current-version accept", "downgrade refusal"),
        "evidence": (
            (ENTERPRISE_CMAKE, ("engine_listener_compatibility_upgrade_downgrade_gate",)),
            (RELEASE_CMAKE, ("public_upgrade_migration_gate",)),
            (DATABASE_LIFECYCLE_CMAKE, ("database_lifecycle_upgrade_migration_conformance",)),
        ),
    },
    {
        "section": "DOC_BACKUP_RESTORE_INTERACTION_GUIDE",
        "topic": "backup_restore",
        "doc_tokens": ("Backup records and archive records are not transaction finality authority", "fail closed on tamper"),
        "evidence": (
            (RELEASE_CMAKE, ("public_backup_forward_session_gate", "public_backup_update_coverage_gate", "public_archive_before_reclaim_gate")),
        ),
    },
    {
        "section": "DOC_LIMITATIONS_SUPPORT_STATES",
        "topic": "limitations_support_states",
        "doc_tokens": ("Linux is the first fully proven platform lane", "cluster-provider-only"),
        "evidence": (
            (SUPPORT_LIFECYCLE_PATH, ("PUBLIC_SUPPORT_RELEASE_LIFECYCLE", "Supported Platforms", "Unsupported Features")),
            (PUBLIC_API_POLICY_PATH, ("PUBLIC_API_COMPATIBILITY_POLICY", "Compatibility Surfaces")),
            (RELEASE_CMAKE, ("public_platform_matrix_gate", "public_unsupported_feature_gate", "public_api_abi_compat_gate")),
        ),
    },
)


def fail(message: str) -> None:
    print(f"public_enterprise_documentation_gate=fail:{message}", file=sys.stderr)
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
            fail(f"private_reference_recorded:{context}:{fragment}")


def read_text(repo_root: Path, relative_path: Path, context: str) -> str:
    path_text = relative_path.as_posix()
    reject_private_reference(path_text, context)
    if relative_path.is_absolute() or ".." in relative_path.parts:
        fail(f"relative_path_invalid:{context}:{path_text}")
    path = repo_root / relative_path
    require(path.is_file(), f"source_missing:{path_text}")
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError as exc:
        fail(f"utf8_decode_failed:{path_text}:{exc}")
    except OSError as exc:
        fail(f"read_failed:{path_text}:{exc}")
    reject_private_reference(text, path_text)
    return text


def require_token(text: str, token: str, context: str) -> None:
    require(isinstance(token, str) and token, f"token_invalid:{context}")
    if token not in text:
        fail(f"token_missing:{context}:{token}")


def validate_policy_tokens(guide_text: str) -> None:
    for token in POLICY_TOKENS:
        require_token(guide_text, token, "enterprise_doc_policy")
    for term in STATUS_TERMS:
        require_token(guide_text, term, "enterprise_doc_support_state")


def validate_row(repo_root: Path, guide_text: str, row: dict[str, Any]) -> dict[str, Any]:
    section = str(row["section"])
    topic = str(row["topic"])
    require_token(guide_text, f"## {section}", f"enterprise_doc_section:{section}")
    for token in row["doc_tokens"]:
        require_token(guide_text, str(token), f"enterprise_doc:{section}")

    source_hashes: list[str] = []
    evidence_token_count = 0
    source_count = 0
    for path, tokens in row["evidence"]:
        path_obj = Path(path)
        source_text = read_text(repo_root, path_obj, f"evidence:{section}")
        source_count += 1
        source_hashes.append(sha256_text(source_text))
        for token in tokens:
            require_token(source_text, str(token), f"evidence:{section}:{path_obj.as_posix()}")
            evidence_token_count += 1

    return {
        "section": section,
        "topic": topic,
        "doc_token_count": str(len(row["doc_tokens"]) + 1),
        "evidence_source_count": str(source_count),
        "evidence_token_count": str(evidence_token_count),
        "source_digest_sha256": sha256_text("|".join(sorted(source_hashes))),
        "status": "pass",
    }


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=[
                "section",
                "topic",
                "doc_token_count",
                "evidence_source_count",
                "evidence_token_count",
                "source_digest_sha256",
                "status",
            ],
        )
        writer.writeheader()
        writer.writerows(rows)


def write_json(path: Path, evidence: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def build_evidence(repo_root: Path) -> dict[str, Any]:
    guide_text = read_text(repo_root, DOC_PATH, "enterprise_doc")
    validate_policy_tokens(guide_text)
    rows = [validate_row(repo_root, guide_text, row) for row in DOC_ROWS]
    topics = {row["topic"] for row in rows}
    required_topics = {str(row["topic"]) for row in DOC_ROWS}
    missing_topics = sorted(required_topics - topics)
    if missing_topics:
        fail("missing_topics:" + ",".join(missing_topics))
    if len(rows) != len(DOC_ROWS):
        fail(f"doc_row_count_drift:{len(rows)}")

    evidence: dict[str, Any] = {
        "schema_version": 1,
        "gate": "PUBLIC_ENTERPRISE_DOCUMENTATION_GATE",
        "policy": {
            "doc_path": DOC_PATH.as_posix(),
            "authority": "public_release_evidence_only",
            "private_execution_plan_references_allowed": False,
            "absolute_local_paths_allowed": False,
            "documentation_is_runtime_authority": False,
        },
        "section_count": len(rows),
        "support_state_terms": list(STATUS_TERMS),
        "guide_sha256": sha256_text(guide_text),
        "rows": rows,
    }
    evidence["evidence_sha256"] = sha256_text(
        json.dumps(evidence, sort_keys=True, separators=(",", ":"))
    )
    return evidence


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--csv-output", required=True, type=Path)
    parser.add_argument("--evidence-output", required=True, type=Path)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    evidence = build_evidence(repo_root)
    write_csv(args.csv_output.resolve(), evidence["rows"])
    write_json(args.evidence_output.resolve(), evidence)
    try:
        output = args.evidence_output.resolve().relative_to(repo_root).as_posix()
    except ValueError:
        output = args.evidence_output.name
    print(f"public_enterprise_documentation_output={output}")
    print(f"public_enterprise_documentation_sha256={evidence['evidence_sha256']}")
    print("public_enterprise_documentation_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
