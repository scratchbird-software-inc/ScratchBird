# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Deterministic audit bundle generation and replay checks."""

from __future__ import annotations

import hmac
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Any

from .deterministic import canonical_json, sha256_hex

REPLAY_OUTCOMES = {
    "match": "REPLAY_MATCH",
    "mismatch_hash": "REPLAY_MISMATCH_HASH",
    "mismatch_policy": "REPLAY_MISMATCH_POLICY",
    "insufficient_data": "REPLAY_INSUFFICIENT_DATA",
}

ATTESTATION_OUTCOMES = {
    "verified": "ATTESTATION_VERIFIED",
    "bundle_mismatch": "ATTESTATION_BUNDLE_MISMATCH",
    "signature_mismatch": "ATTESTATION_SIGNATURE_MISMATCH",
    "invalid": "ATTESTATION_INVALID",
}


def now_utc_iso() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def security_context_hash(security_context: dict[str, Any]) -> str:
    roles_raw = security_context.get("roles", [])
    roles = [str(item) for item in roles_raw] if isinstance(roles_raw, list) else []
    context_version_raw = security_context.get("context_version", 1)
    try:
        context_version = int(context_version_raw)
    except (TypeError, ValueError):
        context_version = 1

    canonical_input = {
        "tenant_id": str(security_context.get("tenant_id", "")),
        "actor_id": str(security_context.get("actor_id", "")),
        "roles": sorted(roles),
        "context_version": max(1, context_version),
    }
    canonical = canonical_json(canonical_input)
    return sha256_hex(canonical)


def approval_token_hash(approval_token: str | None) -> str | None:
    token = (approval_token or "").strip()
    if not token:
        return None
    return sha256_hex(token)


def _bundle_hash_input(bundle: dict[str, Any]) -> dict[str, Any]:
    return {k: v for k, v in bundle.items() if k != "bundle_hash"}


def bundle_hash(bundle: dict[str, Any]) -> str:
    return sha256_hex(canonical_json(_bundle_hash_input(bundle)))


@dataclass(slots=True, frozen=True)
class AuditReplayResult:
    outcome: str
    reason: str


@dataclass(slots=True, frozen=True)
class AuditAttestationResult:
    outcome: str
    reason: str
    verified: bool


def create_audit_bundle(
    *,
    trace_id: str,
    request_id: str,
    tenant_id: str,
    actor_id: str,
    dialect: str,
    execution_mode: str,
    sql_text_normalized: str,
    compile_artifact_id: str,
    execution_artifact_id: str | None,
    plan_json: dict[str, Any] | None,
    plan_hash: str,
    policy_decision: str,
    policy_rule_id: str,
    security_context: dict[str, Any],
    cluster_epoch: int = 0,
    approval_id: str | None = None,
    approval_token: str | None = None,
    cost_attribution: dict[str, Any] | None = None,
    error_code: str | None = None,
    sqlstate: str | None = None,
    created_at_utc: str | None = None,
    bundle_version: str = "1.0",
    statement_kind: str | None = None,
    sblr_hash: str | None = None,
) -> dict[str, Any]:
    sec_hash = security_context_hash(security_context)
    timestamp = created_at_utc or now_utc_iso()
    plan_doc = dict(plan_json) if isinstance(plan_json, dict) else {}
    bundle: dict[str, Any] = {
        "bundle_version": bundle_version,
        "trace_id": trace_id,
        "request_id": request_id,
        "tenant_id": tenant_id,
        "actor_id": actor_id,
        "dialect": dialect,
        "execution_mode": execution_mode,
        "sql_text_normalized": sql_text_normalized,
        "compile_artifact_id": compile_artifact_id,
        "execution_artifact_id": execution_artifact_id,
        "plan_json": plan_doc,
        "plan_hash": plan_hash,
        "security_context_hash": sec_hash,
        "policy_decision": policy_decision,
        "policy_rule_id": policy_rule_id,
        "cluster_epoch": int(cluster_epoch),
        "timestamp_utc": timestamp,
        "approval_id": approval_id,
        "approval_token_hash": approval_token_hash(approval_token),
        "cost_attribution": dict(cost_attribution) if isinstance(cost_attribution, dict) else None,
        "error_code": error_code,
        "sqlstate": sqlstate,
    }
    # Legacy fields retained for transition compatibility.
    bundle["schema_version"] = bundle_version
    bundle["created_at_utc"] = timestamp
    if statement_kind is not None:
        bundle["statement_kind"] = statement_kind
    if sblr_hash is not None:
        bundle["sblr_hash"] = sblr_hash
    bundle["bundle_hash"] = bundle_hash(bundle)
    return bundle


def replay_validate_bundle(
    *,
    bundle: dict[str, Any],
    security_context: dict[str, Any] | None = None,
    expected_policy_decision: str | None = None,
    expected_policy_rule_id: str | None = None,
    expected_plan_hash: str | None = None,
) -> AuditReplayResult:
    required_fields = {
        "bundle_version",
        "request_id",
        "trace_id",
        "tenant_id",
        "actor_id",
        "dialect",
        "execution_mode",
        "sql_text_normalized",
        "compile_artifact_id",
        "plan_json",
        "plan_hash",
        "security_context_hash",
        "policy_decision",
        "policy_rule_id",
        "cluster_epoch",
        "timestamp_utc",
        "bundle_hash",
    }
    if any(field not in bundle for field in required_fields):
        return AuditReplayResult(
            outcome=REPLAY_OUTCOMES["insufficient_data"],
            reason="bundle missing required fields",
        )

    recomputed_bundle_hash = bundle_hash(bundle)
    if recomputed_bundle_hash != bundle.get("bundle_hash"):
        return AuditReplayResult(
            outcome=REPLAY_OUTCOMES["mismatch_hash"],
            reason="bundle hash mismatch",
        )

    if security_context is not None:
        recomputed_sec_hash = security_context_hash(security_context)
        if recomputed_sec_hash != bundle.get("security_context_hash"):
            return AuditReplayResult(
                outcome=REPLAY_OUTCOMES["mismatch_hash"],
                reason="security_context_hash mismatch",
            )

    if expected_policy_decision is not None:
        if expected_policy_decision != bundle.get("policy_decision"):
            return AuditReplayResult(
                outcome=REPLAY_OUTCOMES["mismatch_policy"],
                reason="policy decision mismatch",
            )
    if expected_policy_rule_id is not None:
        if expected_policy_rule_id != bundle.get("policy_rule_id"):
            return AuditReplayResult(
                outcome=REPLAY_OUTCOMES["mismatch_policy"],
                reason="policy rule mismatch",
            )

    if expected_plan_hash is not None:
        if expected_plan_hash != bundle.get("plan_hash"):
            return AuditReplayResult(
                outcome=REPLAY_OUTCOMES["mismatch_hash"],
                reason="plan hash mismatch",
            )

    return AuditReplayResult(
        outcome=REPLAY_OUTCOMES["match"],
        reason="bundle replay checks match",
    )


def _attestation_signature_input(attestation: dict[str, Any]) -> dict[str, Any]:
    return {key: value for key, value in attestation.items() if key != "signature"}


def create_bundle_attestation(
    *,
    bundle: dict[str, Any],
    attestor_id: str,
    attestation_mode: str = "hmac_sha256",
    shared_secret: str | None = None,
    key_id: str | None = None,
    external_reference: str | None = None,
    metadata: dict[str, Any] | None = None,
    issued_at_utc: str | None = None,
) -> dict[str, Any]:
    bundle_hash_value = str(bundle.get("bundle_hash", "")).strip()
    if not bundle_hash_value:
        raise ValueError("bundle_hash is required for attestation")
    normalized_mode = str(attestation_mode).strip() or "hmac_sha256"
    attestation = {
        "attestation_version": "1.0",
        "attestation_mode": normalized_mode,
        "attestor_id": str(attestor_id).strip() or "unknown_attestor",
        "bundle_hash": bundle_hash_value,
        "bundle_trace_id": str(bundle.get("trace_id", "")).strip() or None,
        "issued_at_utc": issued_at_utc or now_utc_iso(),
        "key_id": str(key_id).strip() or None if key_id is not None else None,
        "external_reference": (
            str(external_reference).strip() or None
            if external_reference is not None
            else None
        ),
        "metadata": dict(metadata or {}),
    }
    if normalized_mode == "hmac_sha256":
        secret = str(shared_secret or "").strip()
        if not secret:
            raise ValueError("shared_secret is required for hmac_sha256 attestation")
        payload = canonical_json(_attestation_signature_input(attestation)).encode("utf-8")
        attestation["signature"] = hmac.new(
            secret.encode("utf-8"),
            payload,
            "sha256",
        ).hexdigest()
    elif normalized_mode == "third_party_hmac_sha256":
        secret = str(shared_secret or "").strip()
        if not secret:
            raise ValueError("shared_secret is required for third_party_hmac_sha256 attestation")
        if not attestation["external_reference"]:
            raise ValueError(
                "external_reference is required for third_party_hmac_sha256 attestation"
            )
        if not attestation["key_id"]:
            raise ValueError("key_id is required for third_party_hmac_sha256 attestation")
        attestation["signing_scope"] = "third_party"
        payload = canonical_json(_attestation_signature_input(attestation)).encode("utf-8")
        attestation["signature"] = hmac.new(
            secret.encode("utf-8"),
            payload,
            "sha256",
        ).hexdigest()
    elif normalized_mode == "external_reference":
        if not attestation["external_reference"]:
            raise ValueError("external_reference is required for external_reference attestation")
        attestation["signature"] = None
    else:
        raise ValueError(f"unsupported attestation_mode: {attestation_mode}")
    return attestation


def verify_bundle_attestation(
    *,
    bundle: dict[str, Any],
    attestation: dict[str, Any],
    shared_secret: str | None = None,
) -> AuditAttestationResult:
    if not isinstance(attestation, dict):
        return AuditAttestationResult(
            outcome=ATTESTATION_OUTCOMES["invalid"],
            reason="attestation must be an object",
            verified=False,
        )
    bundle_hash_value = str(bundle.get("bundle_hash", "")).strip()
    attested_bundle_hash = str(attestation.get("bundle_hash", "")).strip()
    if not bundle_hash_value or not attested_bundle_hash:
        return AuditAttestationResult(
            outcome=ATTESTATION_OUTCOMES["invalid"],
            reason="bundle_hash missing from bundle or attestation",
            verified=False,
        )
    if bundle_hash_value != attested_bundle_hash:
        return AuditAttestationResult(
            outcome=ATTESTATION_OUTCOMES["bundle_mismatch"],
            reason="attested bundle_hash does not match bundle",
            verified=False,
        )
    mode = str(attestation.get("attestation_mode", "")).strip()
    if mode == "hmac_sha256":
        secret = str(shared_secret or "").strip()
        signature = str(attestation.get("signature", "")).strip()
        if not secret or not signature:
            return AuditAttestationResult(
                outcome=ATTESTATION_OUTCOMES["invalid"],
                reason="shared_secret and signature are required for hmac_sha256 verification",
                verified=False,
            )
        expected = hmac.new(
            secret.encode("utf-8"),
            canonical_json(_attestation_signature_input(attestation)).encode("utf-8"),
            "sha256",
        ).hexdigest()
        if not hmac.compare_digest(signature, expected):
            return AuditAttestationResult(
                outcome=ATTESTATION_OUTCOMES["signature_mismatch"],
                reason="attestation signature mismatch",
                verified=False,
            )
    elif mode == "third_party_hmac_sha256":
        secret = str(shared_secret or "").strip()
        signature = str(attestation.get("signature", "")).strip()
        if not str(attestation.get("external_reference", "")).strip():
            return AuditAttestationResult(
                outcome=ATTESTATION_OUTCOMES["invalid"],
                reason="third_party_hmac_sha256 attestation is missing external_reference",
                verified=False,
            )
        if not str(attestation.get("key_id", "")).strip():
            return AuditAttestationResult(
                outcome=ATTESTATION_OUTCOMES["invalid"],
                reason="third_party_hmac_sha256 attestation is missing key_id",
                verified=False,
            )
        if not secret or not signature:
            return AuditAttestationResult(
                outcome=ATTESTATION_OUTCOMES["invalid"],
                reason=(
                    "shared_secret and signature are required for "
                    "third_party_hmac_sha256 verification"
                ),
                verified=False,
            )
        expected = hmac.new(
            secret.encode("utf-8"),
            canonical_json(_attestation_signature_input(attestation)).encode("utf-8"),
            "sha256",
        ).hexdigest()
        if not hmac.compare_digest(signature, expected):
            return AuditAttestationResult(
                outcome=ATTESTATION_OUTCOMES["signature_mismatch"],
                reason="attestation signature mismatch",
                verified=False,
            )
    elif mode == "external_reference":
        if not str(attestation.get("external_reference", "")).strip():
            return AuditAttestationResult(
                outcome=ATTESTATION_OUTCOMES["invalid"],
                reason="external_reference attestation is missing external_reference",
                verified=False,
            )
    else:
        return AuditAttestationResult(
            outcome=ATTESTATION_OUTCOMES["invalid"],
            reason=f"unsupported attestation_mode: {mode}",
            verified=False,
        )
    return AuditAttestationResult(
        outcome=ATTESTATION_OUTCOMES["verified"],
        reason="attestation verification passed",
        verified=True,
    )
