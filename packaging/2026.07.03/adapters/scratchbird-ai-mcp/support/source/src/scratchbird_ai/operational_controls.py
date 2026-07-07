# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Operational quotas, rate limits, and cost attribution."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Any

from .tool_schema import ToolContractError


def _utc_now() -> datetime:
    return datetime.now(timezone.utc)


@dataclass(slots=True, frozen=True)
class OperationalCost:
    cost_units: int
    window_started_utc: str
    window_requests: int
    window_mutations: int
    window_cost_units: int

    def to_dict(self) -> dict[str, Any]:
        return {
            "cost_units": self.cost_units,
            "window_started_utc": self.window_started_utc,
            "window_requests": self.window_requests,
            "window_mutations": self.window_mutations,
            "window_cost_units": self.window_cost_units,
        }


@dataclass(slots=True)
class _UsageBucket:
    window_started_utc: str
    requests: int = 0
    mutations: int = 0
    cost_units: int = 0


class OperationalControlEngine:
    """In-process bounded operational control engine."""

    def __init__(
        self,
        *,
        window_sec: int = 60,
        max_requests_per_window: int = 100,
        max_mutations_per_window: int = 20,
        max_cost_units_per_window: int = 1000,
    ) -> None:
        self.window_sec = max(1, int(window_sec))
        self.max_requests_per_window = max(1, int(max_requests_per_window))
        self.max_mutations_per_window = max(1, int(max_mutations_per_window))
        self.max_cost_units_per_window = max(1, int(max_cost_units_per_window))
        self._buckets: dict[tuple[str, str, int], _UsageBucket] = {}

    def _bucket_key(self, *, tenant_id: str, actor_id: str, now: datetime) -> tuple[str, str, int]:
        window_epoch = int(now.timestamp()) // self.window_sec
        return tenant_id, actor_id, window_epoch

    def estimate_cost(self, *, is_mutation: bool, options: dict[str, Any] | None) -> int:
        opts = dict(options or {})
        max_rows = int(opts.get("max_rows", opts.get("limit", 200)) or 200)
        timeout_ms = int(opts.get("timeout_ms", 5000) or 5000)
        memory_mb = int(opts.get("memory_mb", 256) or 256)
        cost_units = 1
        cost_units += max(1, max_rows // 100)
        cost_units += max(1, timeout_ms // 5000)
        cost_units += max(1, memory_mb // 256)
        if is_mutation:
            cost_units += 5
        return cost_units

    def enforce(
        self,
        *,
        tenant_id: str,
        actor_id: str,
        is_mutation: bool,
        options: dict[str, Any] | None,
        now: datetime | None = None,
    ) -> OperationalCost:
        current = now or _utc_now()
        key = self._bucket_key(tenant_id=tenant_id, actor_id=actor_id, now=current)
        bucket = self._buckets.get(key)
        if bucket is None:
            bucket = _UsageBucket(window_started_utc=current.replace(microsecond=0).isoformat().replace("+00:00", "Z"))
            self._buckets[key] = bucket

        cost_units = self.estimate_cost(is_mutation=is_mutation, options=options)
        if bucket.requests + 1 > self.max_requests_per_window:
            raise ToolContractError(
                error_code="E_LIMIT_EXCEEDED",
                message="request rate limit exceeded for current window",
                policy_rule_id="OPS-RATE-001",
            )
        if is_mutation and bucket.mutations + 1 > self.max_mutations_per_window:
            raise ToolContractError(
                error_code="E_LIMIT_EXCEEDED",
                message="mutation rate limit exceeded for current window",
                policy_rule_id="OPS-MUTATION-001",
            )
        if bucket.cost_units + cost_units > self.max_cost_units_per_window:
            raise ToolContractError(
                error_code="E_LIMIT_EXCEEDED",
                message="cost budget exceeded for current window",
                policy_rule_id="OPS-COST-001",
            )

        bucket.requests += 1
        bucket.cost_units += cost_units
        if is_mutation:
            bucket.mutations += 1

        return OperationalCost(
            cost_units=cost_units,
            window_started_utc=bucket.window_started_utc,
            window_requests=bucket.requests,
            window_mutations=bucket.mutations,
            window_cost_units=bucket.cost_units,
        )
