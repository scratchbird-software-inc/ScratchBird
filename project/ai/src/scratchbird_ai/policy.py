# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Policy evaluation for query safety and operation mode controls."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from .execution_mode import (
    ApprovalEvidence,
    ExecutionModeError,
    evaluate_execution_mode,
)


@dataclass(slots=True)
class PolicyDecision:
    allowed: bool
    rule_id: str
    reason: str
    error_code: str | None = None
    canonical_mode: str = "ai_analysis"
    requires_approval: bool = False
    normalized_options: dict[str, int] | None = None


class PolicyDeniedError(RuntimeError):
    """Raised when a request violates platform policy."""

    def __init__(
        self,
        *,
        rule_id: str,
        reason: str,
        error_code: str = "E_POLICY_DENY",
        canonical_mode: str = "ai_analysis",
    ) -> None:
        super().__init__(f"{rule_id}: {reason}")
        self.rule_id = rule_id
        self.reason = reason
        self.error_code = error_code
        self.canonical_mode = canonical_mode


class PolicyEngine:
    """Policy engine with deterministic execution-mode semantics."""

    def evaluate(
        self,
        *,
        mode: str,
        is_mutation: bool,
        approval_token: str | None,
        options: dict[str, Any] | None = None,
        tenant_id: str | None = None,
        actor_id: str | None = None,
        statement_hash: str | None = None,
    ) -> PolicyDecision:
        statement_kind = "mutation" if is_mutation else "read"
        evidence = ApprovalEvidence(approval_token=approval_token)

        try:
            evaluation, _, normalized_options = evaluate_execution_mode(
                mode=mode,
                statement_kind=statement_kind,
                approval=evidence,
                options=options,
                tenant_id=tenant_id,
                actor_id=actor_id,
                statement_hash=statement_hash,
            )
        except ExecutionModeError as exc:
            return PolicyDecision(
                allowed=False,
                rule_id=exc.rule_id,
                reason=exc.message,
                error_code=exc.error_code,
                canonical_mode=exc.canonical_mode,
                requires_approval=exc.canonical_mode != "ai_analysis",
                normalized_options=None,
            )

        return PolicyDecision(
            allowed=evaluation.allowed,
            rule_id=evaluation.rule_id,
            reason=evaluation.reason,
            error_code=evaluation.error_code,
            canonical_mode=evaluation.canonical_mode,
            requires_approval=evaluation.requires_approval,
            normalized_options=normalized_options,
        )

    def enforce(
        self,
        *,
        mode: str,
        is_mutation: bool,
        approval_token: str | None,
        options: dict[str, Any] | None = None,
        tenant_id: str | None = None,
        actor_id: str | None = None,
        statement_hash: str | None = None,
    ) -> PolicyDecision:
        decision = self.evaluate(
            mode=mode,
            is_mutation=is_mutation,
            approval_token=approval_token,
            options=options,
            tenant_id=tenant_id,
            actor_id=actor_id,
            statement_hash=statement_hash,
        )
        if not decision.allowed:
            raise PolicyDeniedError(
                rule_id=decision.rule_id,
                reason=decision.reason,
                error_code=decision.error_code or "E_POLICY_DENY",
                canonical_mode=decision.canonical_mode,
            )
        return decision
