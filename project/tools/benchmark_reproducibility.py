#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Benchmark reproducibility evidence validation helpers.

This module is intentionally standalone: CTest gates may import it without
reading execution_plan, findings, audit, contract, or reference documents.
"""

from __future__ import annotations

import argparse
import json
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable


SCHEMA = "scratchbird.benchmark.reproducibility.v1"
BASELINE_MEASUREMENT_SCHEMA = "scratchbird.beta.performance.measurement.v1"
SPEED_CLAIM_REFUSAL_DIAGNOSTIC = (
    "SB_BENCHMARK_REPRO_REFUSED_MISSING_ENV_OR_VARIANCE: "
    "speed claim requires environment controls and variance evidence"
)


class BenchmarkEvidenceError(ValueError):
    """Raised when benchmark evidence is incomplete or unsupported."""


@dataclass(frozen=True)
class SpeedClaimDecision:
    admitted: bool
    diagnostic: str = ""


def sample_statistics(samples: Iterable[float]) -> dict[str, float | int | list[float]]:
    values = [float(value) for value in samples]
    if not values:
        raise BenchmarkEvidenceError("benchmark sample list is empty")
    mean = sum(values) / len(values)
    variance = sum((value - mean) ** 2 for value in values) / len(values)
    stddev = math.sqrt(variance)
    relative_variance = variance / (mean * mean) if mean != 0.0 else 0.0
    return {
        "repeat_count": len(values),
        "samples": values,
        "mean": mean,
        "variance": variance,
        "stddev": stddev,
        "relative_variance": relative_variance,
        "coefficient_of_variation": stddev / mean if mean != 0.0 else 0.0,
    }


def _has_nonempty_mapping(evidence: dict[str, Any], key: str) -> bool:
    value = evidence.get(key)
    return isinstance(value, dict) and bool(value)


def _has_cache_policy(evidence: dict[str, Any]) -> bool:
    policy = evidence.get("cache_policy")
    return (
        isinstance(policy, dict)
        and bool(policy.get("warm_cache_policy"))
        and bool(policy.get("cold_cache_policy"))
    )


def _has_environment_controls(evidence: dict[str, Any]) -> bool:
    return (
        _has_cache_policy(evidence)
        and _has_nonempty_mapping(evidence, "cpu_thread_controls")
        and _has_nonempty_mapping(evidence, "storage_controls")
        and _has_nonempty_mapping(evidence, "route_controls")
    )


def _has_variance_evidence(evidence: dict[str, Any]) -> bool:
    repeat_count = evidence.get("repeat_count")
    variance = evidence.get("variance_evidence")
    if not isinstance(repeat_count, int) or repeat_count < 2:
        return False
    if not isinstance(variance, dict):
        return False
    if any(key in variance for key in ("variance", "stddev", "relative_variance")):
        return True
    return any(
        isinstance(value, dict)
        and any(key in value for key in ("variance", "stddev", "relative_variance"))
        for value in variance.values()
    )


def admit_speed_claim(evidence: dict[str, Any]) -> SpeedClaimDecision:
    if not evidence.get("speed_claim", False):
        return SpeedClaimDecision(admitted=True)
    if not _has_environment_controls(evidence) or not _has_variance_evidence(evidence):
        return SpeedClaimDecision(False, SPEED_CLAIM_REFUSAL_DIAGNOSTIC)
    return SpeedClaimDecision(admitted=True)


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise BenchmarkEvidenceError(message)


def validate_benchmark_evidence(evidence: dict[str, Any]) -> None:
    decision = admit_speed_claim(evidence)
    if not decision.admitted:
        raise BenchmarkEvidenceError(decision.diagnostic)

    _require(evidence.get("schema") in {SCHEMA, BASELINE_MEASUREMENT_SCHEMA},
             "benchmark evidence schema mismatch")
    _require(_has_cache_policy(evidence), "benchmark evidence missing warm/cold cache policy")
    _require(_has_variance_evidence(evidence), "benchmark evidence missing repeat or variance evidence")
    _require(_has_nonempty_mapping(evidence, "cpu_thread_controls"), "benchmark evidence missing CPU/thread controls")
    _require(_has_nonempty_mapping(evidence, "storage_controls"), "benchmark evidence missing storage controls")
    _require(_has_nonempty_mapping(evidence, "route_controls"), "benchmark evidence missing route controls")
    _require(_has_nonempty_mapping(evidence, "regression_thresholds"), "benchmark evidence missing regression thresholds")

    parity = evidence.get("selected_physical_path_result_parity")
    _require(isinstance(parity, dict), "benchmark evidence missing selected physical path/result parity linkage")
    _require(bool(parity.get("selected_physical_path")), "benchmark evidence missing selected physical path")
    _require(bool(parity.get("selected_result_hash")), "benchmark evidence missing selected result hash")
    _require(parity.get("hash_parity") is True, "benchmark evidence did not prove result hash parity")
    route_hashes = parity.get("route_result_hashes")
    _require(isinstance(route_hashes, dict) and len(route_hashes) >= 2,
             "benchmark evidence missing route result parity hashes")
    _require(len(set(route_hashes.values())) == 1, "benchmark evidence route result hashes differ")

    reproducible = evidence.get("reproducible_json")
    _require(isinstance(reproducible, dict), "benchmark evidence missing reproducible JSON policy")
    _require(reproducible.get("stable_key_order") is True, "benchmark JSON must be emitted with stable key order")
    _require(reproducible.get("runtime_doc_dependencies") == [],
             "benchmark evidence must not depend on docs at runtime")

    authority = evidence.get("authority_controls")
    _require(isinstance(authority, dict), "benchmark evidence missing authority controls")
    _require(authority.get("engine_mga_transaction_authority") is True,
             "benchmark evidence must preserve MGA transaction authority")
    _require(authority.get("donor_or_embedded_storage_transaction_truth") is False,
             "benchmark evidence cannot use donor/embedded storage as transaction truth")


def canonical_json(evidence: dict[str, Any]) -> str:
    validate_benchmark_evidence(evidence)
    return json.dumps(evidence, indent=2, sort_keys=True) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate benchmark reproducibility evidence JSON.")
    parser.add_argument("input", type=Path)
    parser.add_argument("--canonical-output", type=Path)
    args = parser.parse_args()

    evidence = json.loads(args.input.read_text(encoding="utf-8"))
    payload = canonical_json(evidence)
    if args.canonical_output is not None:
        args.canonical_output.parent.mkdir(parents=True, exist_ok=True)
        args.canonical_output.write_text(payload, encoding="utf-8")
    print("benchmark_reproducibility_evidence=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
