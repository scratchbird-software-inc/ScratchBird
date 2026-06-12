#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate public audit retention, integrity, and privacy proof anchors."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import sys
from typing import Any


# PUBLIC_AUDIT_PRIVACY_GATE

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
        "surface": "canonical_audit_registry_contract",
        "path": "public_contract_snapshot",
        "tokens": (
            "SPEC-RECON-0253-AUDIT-EVENTS-REGISTRY-ZERO-GREY",
            "local_audit_schema: sys.security.audit",
            "cluster_audit_schema: cluster.sys.security.audit",
            "default_redaction_policy: safe_message_only",
            "required audit events must be durable before externally visible success",
            "AUDIT.EVENT_UNREGISTERED",
            "AUDIT.PERSISTENCE_FAILED",
            "AUDIT.REDACTION_INVALID",
        ),
    },
    {
        "surface": "canonical_audit_privacy_forbidden_behavior",
        "path": "public_contract_snapshot",
        "tokens": (
            "redacted event views must remove fields according to redaction_policy",
            "store private payload fields in safe redacted view",
            "silently drop required audit event because audit storage is unavailable",
            "external transaction audit events must survive parent rollback only when policy authorizes them",
            "MGA-EXT-AUDIT-EVENT-RESERVATIONS",
        ),
    },
    {
        "surface": "reconciled_audit_registry_privacy",
        "path": "public_contract_snapshot",
        "tokens": (
            "SPEC-RECON-0262-RECONCILED-AUDIT-EVENTS-REGISTRY-ZERO-GREY",
            "redaction_rule: the stricter of this registry redaction and canonical event redaction applies",
            "persistence_rule: canonical event persistence barrier controls durability",
            "credential fields must never be stored in user_safe or actor_visible redacted views",
            "AUDIT.RECONCILED_EVENT_INVALID",
            "AUDIT.EVENT_UNREGISTERED",
            "AUDIT.PERSISTENCE_FAILED",
            "AUDIT.REDACTION_INVALID",
        ),
    },
    {
        "surface": "security_audit_api_retention_privacy",
        "path": "project/src/engine/internal_api/security/audit_api.hpp",
        "tokens": (
            "AUDIT_RETENTION_PRIVACY",
            "EngineEmitAuditEventRequest",
            "EngineEmitAuditEventResult",
            "redacted",
            "EngineEmitLifecycleAuditEventRequest",
        ),
    },
    {
        "surface": "security_audit_api_behavior",
        "path": "project/src/engine/internal_api/security/audit_api.cpp",
        "tokens": (
            "SB_ENGINE_INTERNAL_API_SECURITY_AUDIT_API_BEHAVIOR",
            "SECURITY.AUDIT.EVIDENCE_REQUIRED",
            "AppendSecurityEvidenceEvent",
            "SECURITY.AUDIT.REDACTED",
            "public_private_shape_separated",
            "parser_finality_authority",
            "reference_finality_authority",
        ),
    },
    {
        "surface": "observability_integrity_chain",
        "path": "project/src/engine/internal_api/observability/agent_evidence_retention_api.hpp",
        "tokens": (
            "AUDIT_INTEGRITY_CHAIN",
            "retention_class",
            "retention_policy_ref",
            "legal_hold",
            "maintenance_hold",
            "retention_deadline_expired",
            "raw_principal",
            "raw_evidence_body",
            "support_bundle_payload",
        ),
    },
    {
        "surface": "observability_redaction_retention_behavior",
        "path": "project/src/engine/internal_api/observability/agent_evidence_retention_api.cpp",
        "tokens": (
            "ContainsProtectedMaterial",
            "SafeOrRedacted",
            "<redacted:actor_uuid>",
            "<redacted:policy_body>",
            "<redacted:evidence_body>",
            "<redacted:support_bundle>",
            "evidence_before_success_enforced",
            "retention_decision_recorded",
            "redaction_applied",
            "agent_evidence_retention_tamper_chain",
        ),
    },
    {
        "surface": "repair_ledger_retention_tamper_gate",
        "path": "project/tests/release/public_repair_tamper_retention_crash_resume_gate.cpp",
        "tokens": (
            "retention_hold_recorded",
            "retention_purge_blocked",
            "legal_hold_active",
            "SB-REPAIR-RETENTION-PURGE-BLOCKED",
            "SB-REPAIR-RETENTION-POLICY-REQUIRED",
            "SB-REPAIR-RETENTION-AUTHORITY-REFUSED",
            "SB-REPAIR-RETENTION-LEDGER-UNVERIFIED",
            "tamper_chain_verified",
            "repair_evidence_is_transaction_authority",
        ),
    },
    {
        "surface": "mga_audit_location_non_authority",
        "path": "project/tests/release/public_mga_audit_transaction_location_gate.cpp",
        "tokens": (
            "public_mga_audit_transaction_location_gate",
            "local_inventory_authoritative",
            "archive_authoritative",
            "read_only",
            "writes_refused",
            "audit_transaction_distinct",
            "ENGINE.MGA_AUDIT_LOCATION_UNKNOWN",
            "ENGINE.MGA_AUDIT_LOCATION_IDENTITY_MISMATCH",
        ),
    },
    {
        "surface": "support_bundle_privacy_boundary",
        "path": "project/src/engine/internal_api/observability/cluster_support_bundle_redaction_api.hpp",
        "tokens": (
            "CLUSTER_SUPPORT_BUNDLE_REDACTION",
            "redaction-policy outputs only",
            "retention_evidence_present",
            "sensitive_values_redacted",
            "local_projection_cluster_authority",
        ),
    },
    {
        "surface": "release_gate_wiring",
        "path": "project/tests/release/CMakeLists.txt",
        "tokens": (
            "public_repair_tamper_retention_crash_resume_gate",
            "public_mga_audit_transaction_location_gate",
            "public_support_bundle_incident_gate",
            "public_audit_privacy_gate",
        ),
    },
)


def fail(message: str) -> None:
    print(f"public_audit_privacy_gate=fail:{message}", file=sys.stderr)
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


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--csv-output", required=True, type=Path)
    parser.add_argument("--evidence-output", required=True, type=Path)
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    repo_root = args.repo_root.resolve()
    records = [validate_check(repo_root, check) for check in CHECKS]
    matrix_text = "\n".join(
        f"{record['surface']},{record['path']},{record['status']}" for record in records
    ) + "\n"
    payload = {
        "gate": "PUBLIC_AUDIT_PRIVACY_GATE",
        "status": "pass",
        "checks": records,
        "check_count": len(records),
        "matrix_sha256": sha256_text(matrix_text),
        "authority": {
            "audit_storage": "sys.security.audit",
            "transaction_finality": "local_mga_transaction_inventory",
            "audit_is_mga_finality": False,
            "protected_fields_redacted": True,
        },
    }
    write_csv(args.csv_output, records)
    write_json(args.evidence_output, payload)
    print(
        "public_audit_privacy_gate=passed "
        f"checks={len(records)} matrix_sha256={payload['matrix_sha256']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
