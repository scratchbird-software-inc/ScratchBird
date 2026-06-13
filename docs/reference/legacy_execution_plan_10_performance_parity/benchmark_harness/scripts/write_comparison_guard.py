#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Benchmark comparison eligibility guard.

The harness may run without allowing a firm performance comparison. This helper
keeps that policy explicit and data-driven.
"""

from __future__ import annotations

from typing import Any


def _as_int(value: Any, default: int = 0) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def _runtime_options(payload: dict[str, Any]) -> dict[str, Any]:
    options = payload.get("runtime_options")
    return options if isinstance(options, dict) else {}


def _scratchbird_runtime(payload: dict[str, Any]) -> dict[str, Any]:
    runtime = payload.get("scratchbird_runtime")
    return runtime if isinstance(runtime, dict) else {}


def build_guard(payload: dict[str, Any], environment: dict[str, Any] | None) -> dict[str, Any]:
    options = _runtime_options(payload)
    runtime = _scratchbird_runtime(payload)
    storage_policy = runtime.get("runtime_storage_policy")
    pinning = runtime.get("pinning")
    blocking_reasons: list[str] = []

    executed_runs = _as_int(options.get("executed_runs"))
    minimum_runs = _as_int(options.get("minimum_runs_for_firm_claims"), 5)
    warmup_runs = _as_int(options.get("warmup_runs"))
    if executed_runs < minimum_runs:
        blocking_reasons.append("insufficient executed runs for firm comparison")
    if warmup_runs != 0:
        blocking_reasons.append("warmup runs must be reported separately from measured runs")
    if options.get("best_run_policy") != "forbidden":
        blocking_reasons.append("best-run benchmark claims are forbidden")

    if isinstance(storage_policy, dict) and storage_policy.get("comparison_eligible") is False:
        reason = str(storage_policy.get("reason") or "ScratchBird embedded storage/log artifacts are not eligible")
        blocking_reasons.append(reason)
    if isinstance(pinning, dict) and pinning.get("comparison_eligible") is False:
        reason = str(pinning.get("reason") or "runtime pinning does not support firm comparison")
        blocking_reasons.append(reason)

    if environment is not None:
        cpu_pinned = bool(environment.get("cpu_pinned"))
        storage_isolated = bool(environment.get("storage_isolated"))
        if not cpu_pinned:
            blocking_reasons.append("CPU/thread pinning evidence missing")
        if not storage_isolated:
            blocking_reasons.append("storage isolation evidence missing")

    return {
        "firm_comparison_eligible": not blocking_reasons,
        "blocking_reasons": blocking_reasons,
        "policy": "scratchbird_public_benchmark_comparison_guard_v1",
    }
