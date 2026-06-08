#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate public support and maintenance policy coverage."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import sys
from typing import Any


# PUBLIC_SUPPORT_POLICY_GATE
# PUBLIC_SUPPORT_MAINTENANCE_POLICY

POLICY_DOC = Path("project/docs/release/PUBLIC_SUPPORT_MAINTENANCE_POLICY.md")
LIFECYCLE_DOC = Path("project/docs/release/PUBLIC_SUPPORT_RELEASE_LIFECYCLE.md")
COMPATIBILITY_DOC = Path("project/docs/public_api/CORE_BETA_PUBLIC_COMPATIBILITY_POLICY.md")
RELEASE_CMAKE = Path("project/tests/release/CMakeLists.txt")
ENTERPRISE_CMAKE = Path("project/tests/engine_listener_enterprise/CMakeLists.txt")

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

POLICY_TOKENS = (
    "PUBLIC_SUPPORT_MAINTENANCE_POLICY",
    "public_release_evidence_only",
    "does not define storage authority",
    "transaction finality",
    "authorization",
    "recovery classification",
    "cluster-positive execution authority",
)

POLICY_ROWS: tuple[dict[str, Any], ...] = (
    {
        "section": "SUPPORT_LIFECYCLE_POLICY",
        "topic": "support_lifecycle",
        "doc_tokens": ("Linux proof lane", "Windows x64 and FreeBSD remain target platform lanes"),
        "evidence": (
            (LIFECYCLE_DOC, ("PUBLIC_SUPPORT_RELEASE_LIFECYCLE", "Supported Platforms")),
            (RELEASE_CMAKE, ("public_platform_matrix_gate", "public_release_attestation_gate")),
        ),
    },
    {
        "section": "SECURITY_UPDATE_PROCESS",
        "topic": "security_updates",
        "doc_tokens": ("security fix", "project-tests proof"),
        "evidence": (
            (RELEASE_CMAKE, ("public_enterprise_threat_gate", "public_security_provider_contract_protected_material_gate")),
            (ENTERPRISE_CMAKE, ("engine_listener_adversarial_security_validation_gate",)),
        ),
    },
    {
        "section": "CVE_HANDLING_PROCESS",
        "topic": "cve_handling",
        "doc_tokens": ("affected-version inventory", "fix ownership", "patch artifact tracking"),
        "evidence": (
            (RELEASE_CMAKE, ("public_dependency_sbom_gate", "public_artifact_signature_gate", "public_release_attestation_gate")),
        ),
    },
    {
        "section": "DISCLOSURE_POLICY",
        "topic": "disclosure",
        "doc_tokens": ("coordinated disclosure", "redacted public advisory text"),
        "evidence": (
            (RELEASE_CMAKE, ("public_audit_privacy_gate", "public_crypto_entropy_policy_gate")),
            (ENTERPRISE_CMAKE, ("engine_listener_support_bundle_redaction_gate",)),
        ),
    },
    {
        "section": "PATCH_RELEASE_PROCESS",
        "topic": "patch_release",
        "doc_tokens": ("deterministic version metadata", "SBOM/license/vulnerability scan"),
        "evidence": (
            (RELEASE_CMAKE, ("public_release_version_metadata_gate", "public_artifact_reproducibility_gate", "public_artifact_signature_gate")),
        ),
    },
    {
        "section": "COMPATIBILITY_POLICY",
        "topic": "compatibility",
        "doc_tokens": ("Unsupported old formats", "downgrade requests", "stable diagnostics"),
        "evidence": (
            (COMPATIBILITY_DOC, ("PUBLIC_API_COMPATIBILITY_POLICY", "Compatibility Surfaces")),
            (RELEASE_CMAKE, ("public_api_abi_compat_gate",)),
            (ENTERPRISE_CMAKE, ("engine_listener_compatibility_upgrade_downgrade_gate",)),
        ),
    },
    {
        "section": "DATA_LOSS_ESCALATION_POLICY",
        "topic": "data_loss_escalation",
        "doc_tokens": ("Potential data-loss issues", "fence writes"),
        "evidence": (
            (ENTERPRISE_CMAKE, ("engine_listener_crash_recovery_certification_gate",)),
            (RELEASE_CMAKE, ("public_disaster_recovery_gate", "public_backup_update_coverage_gate")),
        ),
    },
    {
        "section": "RELEASE_ROLLBACK_POLICY",
        "topic": "release_rollback",
        "doc_tokens": ("downgrade-refusal boundary", "unsupported durable-format downgrade"),
        "evidence": (
            (RELEASE_CMAKE, ("public_upgrade_migration_gate", "public_disaster_recovery_gate", "public_admin_runbook_gate")),
        ),
    },
    {
        "section": "DIAGNOSTIC_COLLECTION_POLICY",
        "topic": "diagnostic_collection",
        "doc_tokens": ("redacted support bundles", "stable diagnostic codes", "private local paths"),
        "evidence": (
            (RELEASE_CMAKE, ("public_support_bundle_incident_gate", "public_diagnostic_stability_gate", "public_audit_privacy_gate")),
        ),
    },
    {
        "section": "ENTERPRISE_SLA_BOUNDARIES",
        "topic": "sla_boundaries",
        "doc_tokens": ("supported release lines", "contracted package artifacts", "unsupported feature families"),
        "evidence": (
            (LIFECYCLE_DOC, ("PUBLIC_SUPPORT_RELEASE_LIFECYCLE", "First-Release Support Boundaries")),
            (RELEASE_CMAKE, ("public_platform_matrix_gate", "public_unsupported_feature_gate")),
        ),
    },
    {
        "section": "ALPHA_BETA_GA_SUPPORT_BOUNDARIES",
        "topic": "release_stage_boundaries",
        "doc_tokens": ("Alpha and developer-preview builds", "GA support requires final gold aggregation"),
        "evidence": (
            (ENTERPRISE_CMAKE, ("engine_listener_enterprise_documentation_gate", "engine_listener_release_artifact_trust_gate", "engine_listener_enterprise_foundation_check")),
        ),
    },
    {
        "section": "COMMUNITY_COMMERCIAL_SUPPORT_BOUNDARY",
        "topic": "community_commercial",
        "doc_tokens": ("Community support", "Commercial support", "regenerated proof"),
        "evidence": (
            (RELEASE_CMAKE, ("public_support_policy_gate", "public_enterprise_documentation_gate", "public_support_bundle_incident_gate")),
        ),
    },
)


def fail(message: str) -> None:
    print(f"public_support_policy_gate=fail:{message}", file=sys.stderr)
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


def validate_policy_tokens(policy_text: str) -> None:
    for token in POLICY_TOKENS:
        require_token(policy_text, token, "support_policy")


def validate_row(repo_root: Path, policy_text: str, row: dict[str, Any]) -> dict[str, Any]:
    section = str(row["section"])
    topic = str(row["topic"])
    require_token(policy_text, f"## {section}", f"support_policy_section:{section}")
    for token in row["doc_tokens"]:
        require_token(policy_text, str(token), f"support_policy:{section}")

    source_count = 0
    evidence_token_count = 0
    source_hashes: list[str] = []
    for path, tokens in row["evidence"]:
        source_text = read_text(repo_root, Path(path), f"evidence:{section}")
        source_count += 1
        source_hashes.append(sha256_text(source_text))
        for token in tokens:
            require_token(source_text, str(token), f"evidence:{section}:{Path(path).as_posix()}")
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
    policy_text = read_text(repo_root, POLICY_DOC, "support_policy")
    validate_policy_tokens(policy_text)
    rows = [validate_row(repo_root, policy_text, row) for row in POLICY_ROWS]
    if len(rows) != len(POLICY_ROWS):
        fail(f"support_policy_row_count_drift:{len(rows)}")
    evidence: dict[str, Any] = {
        "schema_version": 1,
        "gate": "PUBLIC_SUPPORT_POLICY_GATE",
        "policy": {
            "doc_path": POLICY_DOC.as_posix(),
            "authority": "public_release_evidence_only",
            "private_execution_plan_references_allowed": False,
            "absolute_local_paths_allowed": False,
            "support_policy_is_runtime_authority": False,
        },
        "section_count": len(rows),
        "policy_sha256": sha256_text(policy_text),
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
    print(f"public_support_policy_output={output}")
    print(f"public_support_policy_sha256={evidence['evidence_sha256']}")
    print("public_support_policy_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
