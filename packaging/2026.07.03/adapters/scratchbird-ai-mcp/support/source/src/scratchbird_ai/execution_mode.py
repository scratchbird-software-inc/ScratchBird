# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Execution mode state machine and approval/limit enforcement."""

from __future__ import annotations

import json
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Any

from .deterministic import deterministic_id

CANONICAL_MODES = {
    "ai_analysis",
    "ai_mutation_pending_approval",
    "ai_mutation_approved",
}

MODE_ALIASES = {
    "read_only": "ai_analysis",
    "mutation_with_approval": "ai_mutation_pending_approval",
}

MODE_ERRORS = {
    "invalid_mode": "E_INVALID_MODE",
    "approval_invalid": "E_APPROVAL_INVALID",
    "policy_deny": "E_POLICY_DENY",
    "limit_exceeded": "E_LIMIT_EXCEEDED",
}


@dataclass(slots=True, frozen=True)
class ResourceLimits:
    default_max_rows: int = 200
    default_timeout_ms: int = 5000
    default_memory_mb: int = 256
    hard_max_rows: int = 10_000
    hard_timeout_ms: int = 30_000
    hard_memory_mb: int = 2_048


@dataclass(slots=True, frozen=True)
class ApprovalEvidence:
    approval_token: str | None = None
    approval_id: str | None = None
    approved_by: str | None = None
    approved_at: str | None = None


@dataclass(slots=True, frozen=True)
class ModeEvaluation:
    allowed: bool
    canonical_mode: str
    statement_kind: str
    rule_id: str
    reason: str
    error_code: str | None = None
    requires_approval: bool = False


class ExecutionModeError(RuntimeError):
    def __init__(
        self,
        *,
        error_code: str,
        rule_id: str,
        message: str,
        canonical_mode: str,
    ) -> None:
        super().__init__(message)
        self.error_code = error_code
        self.rule_id = rule_id
        self.message = message
        self.canonical_mode = canonical_mode


def _to_int(value: Any, default: int) -> int:
    if value is None:
        return default
    if isinstance(value, bool):
        return default
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def _now_utc_iso() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def _parse_token_claims(token: str) -> dict[str, Any]:
    token = token.strip()
    if not token:
        return {}
    if token.startswith("{") and token.endswith("}"):
        try:
            payload = json.loads(token)
        except json.JSONDecodeError:
            return {}
        if isinstance(payload, dict):
            return payload
    return {}


def normalize_mode(mode: str, *, approval_present: bool = False) -> str:
    raw = (mode or "").strip().lower()
    if not raw:
        return "ai_analysis"

    mapped = MODE_ALIASES.get(raw, raw)
    if raw == "mutation_with_approval" and approval_present:
        # Legacy alias with explicit evidence upgrades to approved mode.
        mapped = "ai_mutation_approved"

    if mapped not in CANONICAL_MODES:
        raise ExecutionModeError(
            error_code=MODE_ERRORS["invalid_mode"],
            rule_id="MODE-INVALID-001",
            message=f"Unsupported execution mode: {mode}",
            canonical_mode="ai_analysis",
        )
    return mapped


def normalize_options(
    *,
    options: dict[str, Any] | None,
    limits: ResourceLimits | None = None,
) -> dict[str, int]:
    lim = limits or ResourceLimits()
    opts = options or {}

    normalized = {
        "max_rows": _to_int(opts.get("max_rows"), lim.default_max_rows),
        "timeout_ms": _to_int(opts.get("timeout_ms"), lim.default_timeout_ms),
        "memory_mb": _to_int(opts.get("memory_mb"), lim.default_memory_mb),
    }

    if (
        normalized["max_rows"] > lim.hard_max_rows
        or normalized["timeout_ms"] > lim.hard_timeout_ms
        or normalized["memory_mb"] > lim.hard_memory_mb
    ):
        raise ExecutionModeError(
            error_code=MODE_ERRORS["limit_exceeded"],
            rule_id="MODE-LIMIT-001",
            message="Execution options exceed hard limits",
            canonical_mode="ai_analysis",
        )

    if normalized["max_rows"] < 1:
        normalized["max_rows"] = 1
    if normalized["timeout_ms"] < 100:
        normalized["timeout_ms"] = 100
    if normalized["memory_mb"] < 64:
        normalized["memory_mb"] = 64

    return normalized


def validate_approval(
    *,
    approval: ApprovalEvidence | None,
    tenant_id: str | None = None,
    actor_id: str | None = None,
    statement_hash: str | None = None,
    now_utc: str | None = None,
) -> ApprovalEvidence:
    evidence = approval or ApprovalEvidence()
    token = (evidence.approval_token or "").strip()
    if not token:
        raise ExecutionModeError(
            error_code=MODE_ERRORS["approval_invalid"],
            rule_id="MODE-APPROVAL-001",
            message="Missing approval token",
            canonical_mode="ai_mutation_approved",
        )

    claims = _parse_token_claims(token)
    if claims:
        now_iso = now_utc or _now_utc_iso()
        now_dt = datetime.fromisoformat(now_iso.replace("Z", "+00:00"))
        exp_raw = claims.get("exp")
        if exp_raw is not None:
            try:
                exp_dt = datetime.fromisoformat(str(exp_raw).replace("Z", "+00:00"))
            except ValueError:
                raise ExecutionModeError(
                    error_code=MODE_ERRORS["approval_invalid"],
                    rule_id="MODE-APPROVAL-002",
                    message="Invalid approval token exp claim format",
                    canonical_mode="ai_mutation_approved",
                ) from None
            if exp_dt <= now_dt:
                raise ExecutionModeError(
                    error_code=MODE_ERRORS["approval_invalid"],
                    rule_id="MODE-APPROVAL-003",
                    message="Approval token expired",
                    canonical_mode="ai_mutation_approved",
                )

        claim_checks = {
            "tenant_id": tenant_id,
            "actor_id": actor_id,
            "statement_hash": statement_hash,
        }
        for key, expected in claim_checks.items():
            if expected is None:
                continue
            actual = claims.get(key)
            if actual is None:
                continue
            if str(actual) != str(expected):
                raise ExecutionModeError(
                    error_code=MODE_ERRORS["approval_invalid"],
                    rule_id="MODE-APPROVAL-004",
                    message=f"Approval token claim mismatch for {key}",
                    canonical_mode="ai_mutation_approved",
                )

    approval_id = evidence.approval_id or deterministic_id(
        "appr", {"token": token, "tenant_id": tenant_id or "", "actor_id": actor_id or ""}
    )
    approved_at = evidence.approved_at or _now_utc_iso()
    return ApprovalEvidence(
        approval_token=token,
        approval_id=approval_id,
        approved_by=evidence.approved_by or actor_id or "unknown",
        approved_at=approved_at,
    )


def evaluate_execution_mode(
    *,
    mode: str,
    statement_kind: str,
    approval: ApprovalEvidence | None = None,
    options: dict[str, Any] | None = None,
    tenant_id: str | None = None,
    actor_id: str | None = None,
    statement_hash: str | None = None,
    limits: ResourceLimits | None = None,
) -> tuple[ModeEvaluation, ApprovalEvidence | None, dict[str, int]]:
    approval_present = bool((approval.approval_token if approval else None))
    canonical_mode = normalize_mode(mode, approval_present=approval_present)
    normalized_options = normalize_options(options=options, limits=limits)

    kind = statement_kind if statement_kind in {"read", "mutation"} else "unknown"
    if kind == "read":
        return (
            ModeEvaluation(
                allowed=True,
                canonical_mode=canonical_mode,
                statement_kind=kind,
                rule_id="MODE-ALLOW-READ-001",
                reason="Non-mutation statement allowed",
                requires_approval=False,
            ),
            None,
            normalized_options,
        )

    # Unknown statements are treated as mutation for fail-closed safety.
    if canonical_mode == "ai_analysis":
        return (
            ModeEvaluation(
                allowed=False,
                canonical_mode=canonical_mode,
                statement_kind=kind,
                rule_id="MODE-DENY-MUTATION-ANALYSIS-001",
                reason="Mutations are denied in ai_analysis mode",
                error_code=MODE_ERRORS["policy_deny"],
                requires_approval=True,
            ),
            None,
            normalized_options,
        )

    if canonical_mode == "ai_mutation_pending_approval":
        return (
            ModeEvaluation(
                allowed=False,
                canonical_mode=canonical_mode,
                statement_kind=kind,
                rule_id="MODE-DENY-MUTATION-PENDING-001",
                reason="Mutation is pending approval evidence",
                error_code=MODE_ERRORS["policy_deny"],
                requires_approval=True,
            ),
            None,
            normalized_options,
        )

    validated = validate_approval(
        approval=approval,
        tenant_id=tenant_id,
        actor_id=actor_id,
        statement_hash=statement_hash,
    )
    return (
        ModeEvaluation(
            allowed=True,
            canonical_mode=canonical_mode,
            statement_kind=kind,
            rule_id="MODE-ALLOW-MUTATION-APPROVED-001",
            reason="Mutation allowed with validated approval evidence",
            requires_approval=True,
        ),
        validated,
        normalized_options,
    )


def validate_transition(
    *,
    from_mode: str,
    to_mode: str,
    approval: ApprovalEvidence | None = None,
) -> bool:
    source = normalize_mode(from_mode, approval_present=bool(approval and approval.approval_token))
    target = normalize_mode(to_mode, approval_present=bool(approval and approval.approval_token))

    allowed_transitions = {
        ("ai_analysis", "ai_mutation_pending_approval"),
        ("ai_mutation_pending_approval", "ai_mutation_approved"),
        ("ai_mutation_approved", "ai_analysis"),
    }
    if (source, target) not in allowed_transitions:
        return False
    if source == "ai_mutation_pending_approval" and target == "ai_mutation_approved":
        try:
            validate_approval(approval=approval)
        except ExecutionModeError:
            return False
    return True
