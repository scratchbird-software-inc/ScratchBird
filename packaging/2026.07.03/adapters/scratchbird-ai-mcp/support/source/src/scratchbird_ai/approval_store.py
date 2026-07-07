# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Durable approval evidence ledger for governed mutation execution."""

from __future__ import annotations

import json
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from .audit_bundle import approval_token_hash
from .deterministic import deterministic_id
from .tool_schema import ToolContractError


def _utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def _parse_timestamp(value: str | None) -> datetime | None:
    if not value:
        return None
    try:
        return datetime.fromisoformat(str(value).replace("Z", "+00:00"))
    except ValueError:
        return None


def _token_claims(token: str | None) -> dict[str, Any]:
    raw = (token or "").strip()
    if not raw.startswith("{") or not raw.endswith("}"):
        return {}
    try:
        payload = json.loads(raw)
    except json.JSONDecodeError:
        return {}
    return payload if isinstance(payload, dict) else {}


@dataclass(slots=True)
class ApprovalRecord:
    approval_id: str
    approval_token_hash: str
    tenant_id: str
    actor_id: str
    statement_hash: str
    approved_by: str
    approved_at: str
    expires_at: str | None = None
    revoked_at: str | None = None
    revoked_by: str | None = None
    revocation_reason: str | None = None
    last_used_at: str | None = None
    use_count: int = 0

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)

    @classmethod
    def from_dict(cls, payload: dict[str, Any]) -> "ApprovalRecord":
        return cls(
            approval_id=str(payload["approval_id"]),
            approval_token_hash=str(payload["approval_token_hash"]),
            tenant_id=str(payload.get("tenant_id", "")),
            actor_id=str(payload.get("actor_id", "")),
            statement_hash=str(payload.get("statement_hash", "")),
            approved_by=str(payload.get("approved_by", "unknown")),
            approved_at=str(payload.get("approved_at", "")),
            expires_at=(str(payload["expires_at"]) if payload.get("expires_at") else None),
            revoked_at=(str(payload["revoked_at"]) if payload.get("revoked_at") else None),
            revoked_by=(str(payload["revoked_by"]) if payload.get("revoked_by") else None),
            revocation_reason=(
                str(payload["revocation_reason"])
                if payload.get("revocation_reason")
                else None
            ),
            last_used_at=(str(payload["last_used_at"]) if payload.get("last_used_at") else None),
            use_count=int(payload.get("use_count", 0) or 0),
        )


class ApprovalLedger:
    """Small file-backed approval ledger used for replay-safe mutation approval."""

    def __init__(self, *, path: str | None = None) -> None:
        self._path = Path(path).expanduser() if path else None
        self._records: dict[str, ApprovalRecord] = {}
        self._loaded = False

    def _load(self) -> None:
        if self._loaded or self._path is None:
            self._loaded = True
            return
        if self._path.exists():
            raw = json.loads(self._path.read_text(encoding="utf-8"))
            if isinstance(raw, dict):
                for approval_id, payload in raw.items():
                    if isinstance(payload, dict):
                        self._records[str(approval_id)] = ApprovalRecord.from_dict(payload)
        self._loaded = True

    def _persist(self) -> None:
        if self._path is None:
            return
        self._path.parent.mkdir(parents=True, exist_ok=True)
        data = {
            approval_id: record.to_dict()
            for approval_id, record in sorted(self._records.items())
        }
        tmp_path = self._path.with_suffix(f"{self._path.suffix}.tmp")
        tmp_path.write_text(json.dumps(data, indent=2, sort_keys=True), encoding="utf-8")
        tmp_path.replace(self._path)

    def get(self, approval_id: str) -> ApprovalRecord | None:
        self._load()
        return self._records.get(approval_id)

    def revoke(
        self,
        approval_id: str,
        *,
        reason: str,
        revoked_at: str | None = None,
        revoked_by: str | None = None,
    ) -> ApprovalRecord:
        self._load()
        record = self._records.get(approval_id)
        if record is None:
            raise KeyError(f"unknown approval_id: {approval_id}")
        record.revoked_at = revoked_at or _utc_now()
        record.revoked_by = str(revoked_by).strip() or None if revoked_by is not None else None
        record.revocation_reason = reason
        self._persist()
        return record

    def list_records(
        self,
        *,
        tenant_id: str | None = None,
        actor_id: str | None = None,
        include_revoked: bool = True,
    ) -> list[ApprovalRecord]:
        self._load()
        rows: list[ApprovalRecord] = []
        tenant_filter = str(tenant_id or "").strip()
        actor_filter = str(actor_id or "").strip()
        for record in self._records.values():
            if tenant_filter and record.tenant_id != tenant_filter:
                continue
            if actor_filter and record.actor_id != actor_filter:
                continue
            if not include_revoked and record.revoked_at:
                continue
            rows.append(record)
        rows.sort(key=lambda item: (item.approved_at, item.approval_id))
        return rows

    def summary(self, *, now_utc: str | None = None) -> dict[str, Any]:
        self._load()
        now_dt = _parse_timestamp(now_utc or _utc_now())
        revoked = 0
        expired = 0
        active = 0
        for record in self._records.values():
            if record.revoked_at:
                revoked += 1
                continue
            expires_at = _parse_timestamp(record.expires_at)
            if now_dt is not None and expires_at is not None and expires_at <= now_dt:
                expired += 1
                continue
            active += 1
        return {
            "total_records": len(self._records),
            "active_records": active,
            "revoked_records": revoked,
            "expired_records": expired,
        }

    def validate_or_register(
        self,
        *,
        approval_token: str,
        approval_evidence: dict[str, Any] | None,
        tenant_id: str,
        actor_id: str,
        statement_hash: str,
        now_utc: str | None = None,
    ) -> ApprovalRecord:
        self._load()
        now_value = now_utc or _utc_now()
        token_hash = approval_token_hash(approval_token)
        if token_hash is None:
            raise ToolContractError(
                error_code="E_APPROVAL_INVALID",
                message="approval token is required for durable approval validation",
                policy_rule_id="MODE-APPROVAL-LEDGER-001",
            )

        evidence = dict(approval_evidence or {})
        provided_approval_id = str(evidence.get("approval_id", "")).strip()
        approval_id = provided_approval_id or deterministic_id(
            "appr",
            {
                "token_hash": token_hash,
                "tenant_id": tenant_id,
                "actor_id": actor_id,
                "statement_hash": statement_hash,
            },
        )

        record = self._records.get(approval_id)
        if record is None:
            claims = _token_claims(approval_token)
            expires_at = str(evidence.get("expires_at", "")).strip() or None
            if expires_at is None and claims.get("exp"):
                expires_at = str(claims["exp"]).strip() or None
            approved_by = (
                str(evidence.get("approved_by", "")).strip()
                or str(claims.get("approved_by", "")).strip()
                or actor_id
                or "unknown"
            )
            approved_at = (
                str(evidence.get("approved_at", "")).strip()
                or str(claims.get("approved_at", "")).strip()
                or now_value
            )
            record = ApprovalRecord(
                approval_id=approval_id,
                approval_token_hash=token_hash,
                tenant_id=tenant_id,
                actor_id=actor_id,
                statement_hash=statement_hash,
                approved_by=approved_by,
                approved_at=approved_at,
                expires_at=expires_at,
            )
            self._records[approval_id] = record

        if record.approval_token_hash != token_hash:
            raise ToolContractError(
                error_code="E_APPROVAL_INVALID",
                message="approval token hash does not match stored approval evidence",
                policy_rule_id="MODE-APPROVAL-LEDGER-002",
            )
        if record.tenant_id and record.tenant_id != tenant_id:
            raise ToolContractError(
                error_code="E_APPROVAL_INVALID",
                message="approval evidence tenant mismatch",
                policy_rule_id="MODE-APPROVAL-LEDGER-003",
            )
        if record.actor_id and record.actor_id != actor_id:
            raise ToolContractError(
                error_code="E_APPROVAL_INVALID",
                message="approval evidence actor mismatch",
                policy_rule_id="MODE-APPROVAL-LEDGER-004",
            )
        if record.statement_hash and record.statement_hash != statement_hash:
            raise ToolContractError(
                error_code="E_APPROVAL_INVALID",
                message="approval evidence statement mismatch",
                policy_rule_id="MODE-APPROVAL-LEDGER-005",
            )
        if record.revoked_at:
            raise ToolContractError(
                error_code="E_APPROVAL_INVALID",
                message="approval evidence has been revoked",
                policy_rule_id="MODE-APPROVAL-LEDGER-006",
            )
        expires_at = _parse_timestamp(record.expires_at)
        now_dt = _parse_timestamp(now_value)
        if expires_at is None and record.expires_at:
            raise ToolContractError(
                error_code="E_APPROVAL_INVALID",
                message="approval evidence has invalid expires_at format",
                policy_rule_id="MODE-APPROVAL-LEDGER-007",
            )
        if now_dt is not None and expires_at is not None and expires_at <= now_dt:
            raise ToolContractError(
                error_code="E_APPROVAL_INVALID",
                message="approval evidence is expired",
                policy_rule_id="MODE-APPROVAL-LEDGER-008",
            )

        record.last_used_at = now_value
        record.use_count += 1
        self._persist()
        return record
