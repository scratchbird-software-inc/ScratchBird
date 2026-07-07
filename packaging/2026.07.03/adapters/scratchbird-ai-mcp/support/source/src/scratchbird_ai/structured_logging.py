# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Structured runtime event logging and summarization helpers."""

from __future__ import annotations

import json
import math
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


def _utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def _safe_float(value: Any) -> float | None:
    try:
        out = float(value)
    except (TypeError, ValueError):
        return None
    if not math.isfinite(out):
        return None
    return out


def _normalized_security_context(security_context: dict[str, Any] | None) -> dict[str, str]:
    if not isinstance(security_context, dict):
        return {"tenant_id": "", "actor_id": ""}
    return {
        "tenant_id": str(security_context.get("tenant_id", "")).strip(),
        "actor_id": str(security_context.get("actor_id", "")).strip(),
    }


def _percentile(values: list[float], percentile: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    position = max(0.0, min(1.0, percentile)) * (len(ordered) - 1)
    lower = int(math.floor(position))
    upper = int(math.ceil(position))
    if lower == upper:
        return ordered[lower]
    fraction = position - lower
    return ordered[lower] + (ordered[upper] - ordered[lower]) * fraction


@dataclass(slots=True, frozen=True)
class StructuredEvent:
    timestamp_utc: str
    event_type: str
    status: str
    trace_id: str | None
    request_id: str | None
    interface_profile_id: str | None
    tenant_id: str
    actor_id: str
    duration_ms: float | None
    attributes: dict[str, Any]

    def to_dict(self) -> dict[str, Any]:
        return {
            "timestamp_utc": self.timestamp_utc,
            "event_type": self.event_type,
            "status": self.status,
            "trace_id": self.trace_id,
            "request_id": self.request_id,
            "interface_profile_id": self.interface_profile_id,
            "tenant_id": self.tenant_id,
            "actor_id": self.actor_id,
            "duration_ms": self.duration_ms,
            "attributes": dict(self.attributes),
        }


class StructuredEventLogger:
    """Append-only JSONL event logger used for operator diagnostics."""

    def __init__(self, *, path: str | None = None) -> None:
        self._path = Path(path).expanduser() if path else None

    @property
    def path(self) -> Path | None:
        return self._path

    def emit(
        self,
        *,
        event_type: str,
        status: str,
        trace_id: str | None = None,
        request_id: str | None = None,
        interface_profile_id: str | None = None,
        security_context: dict[str, Any] | None = None,
        duration_ms: float | None = None,
        attributes: dict[str, Any] | None = None,
        timestamp_utc: str | None = None,
    ) -> dict[str, Any]:
        event = StructuredEvent(
            timestamp_utc=timestamp_utc or _utc_now(),
            event_type=str(event_type).strip() or "unknown",
            status=str(status).strip() or "unknown",
            trace_id=str(trace_id).strip() or None if trace_id is not None else None,
            request_id=str(request_id).strip() or None if request_id is not None else None,
            interface_profile_id=(
                str(interface_profile_id).strip() or None
                if interface_profile_id is not None
                else None
            ),
            tenant_id=_normalized_security_context(security_context)["tenant_id"],
            actor_id=_normalized_security_context(security_context)["actor_id"],
            duration_ms=round(duration_ms, 6) if duration_ms is not None else None,
            attributes=dict(attributes or {}),
        )
        payload = event.to_dict()
        if self._path is not None:
            self._path.parent.mkdir(parents=True, exist_ok=True)
            with self._path.open("a", encoding="utf-8") as handle:
                handle.write(json.dumps(payload, sort_keys=True) + "\n")
        return payload

    def load_events(self) -> list[dict[str, Any]]:
        if self._path is None or not self._path.exists():
            return []
        rows: list[dict[str, Any]] = []
        for raw_line in self._path.read_text(encoding="utf-8").splitlines():
            line = raw_line.strip()
            if not line:
                continue
            try:
                payload = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(payload, dict):
                rows.append(payload)
        return rows

    def summarize(self, *, max_recent_errors: int = 10) -> dict[str, Any]:
        return summarize_events(self.load_events(), max_recent_errors=max_recent_errors)


def summarize_events(
    events: list[dict[str, Any]],
    *,
    max_recent_errors: int = 10,
) -> dict[str, Any]:
    event_type_counts: dict[str, int] = {}
    status_counts: dict[str, int] = {}
    error_code_counts: dict[str, int] = {}
    policy_rule_counts: dict[str, int] = {}
    durations_ms: list[float] = []
    recent_errors: list[dict[str, Any]] = []

    for payload in events:
        event_type = str(payload.get("event_type", "")).strip() or "unknown"
        status = str(payload.get("status", "")).strip() or "unknown"
        event_type_counts[event_type] = event_type_counts.get(event_type, 0) + 1
        status_counts[status] = status_counts.get(status, 0) + 1
        duration_ms = _safe_float(payload.get("duration_ms"))
        if duration_ms is not None:
            durations_ms.append(duration_ms)
        attributes = payload.get("attributes", {})
        if not isinstance(attributes, dict):
            attributes = {}
        error_code = str(attributes.get("error_code", "")).strip()
        if error_code:
            error_code_counts[error_code] = error_code_counts.get(error_code, 0) + 1
        policy_rule_id = str(attributes.get("policy_rule_id", "")).strip()
        if policy_rule_id:
            policy_rule_counts[policy_rule_id] = policy_rule_counts.get(policy_rule_id, 0) + 1
        if status in {"error", "denied"}:
            recent_errors.append(
                {
                    "timestamp_utc": payload.get("timestamp_utc"),
                    "event_type": event_type,
                    "status": status,
                    "trace_id": payload.get("trace_id"),
                    "request_id": payload.get("request_id"),
                    "error_code": error_code or None,
                    "policy_rule_id": policy_rule_id or None,
                    "message": attributes.get("message"),
                }
            )

    total_events = len(events)
    error_events = status_counts.get("error", 0) + status_counts.get("denied", 0)
    mean_duration_ms = (
        round(sum(durations_ms) / len(durations_ms), 6) if durations_ms else 0.0
    )
    p95_duration_ms = round(_percentile(durations_ms, 0.95), 6) if durations_ms else 0.0
    return {
        "total_events": total_events,
        "status_counts": dict(sorted(status_counts.items())),
        "event_type_counts": dict(sorted(event_type_counts.items())),
        "error_events": error_events,
        "error_rate_pct": round((error_events / total_events) * 100.0, 6)
        if total_events
        else 0.0,
        "mean_duration_ms": mean_duration_ms,
        "p95_duration_ms": p95_duration_ms,
        "top_error_codes": dict(
            sorted(error_code_counts.items(), key=lambda item: (-item[1], item[0]))
        ),
        "top_policy_rule_ids": dict(
            sorted(policy_rule_counts.items(), key=lambda item: (-item[1], item[0]))
        ),
        "recent_errors": recent_errors[-max_recent_errors:],
    }
