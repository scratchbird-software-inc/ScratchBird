#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""ORH-270 donor-dominance closure validator.

This gate intentionally does not synthesize benchmark results. It validates
that ORH-270 is closed only as completed-blocked unless fresh live ScratchBird
and donor-best dominance artifacts are present.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


REQUIRED_ROUTES = {"embedded", "ipc", "inet"}
REQUIRED_DONORS = {"firebird", "mysql", "postgresql"}
REQUIRED_NEGATIVE_DIAGNOSTICS = {
    "ORH_DONOR_DOMINANCE_STALE_ARTIFACT",
    "ORH_DONOR_DOMINANCE_DONOR_CONTROLS_MISSING",
    "ORH_DONOR_DOMINANCE_LIVE_ROUTE_MISSING.IPC",
    "ORH_DONOR_DOMINANCE_LIVE_ROUTE_MISSING.INET",
    "ORH_DONOR_DOMINANCE_RESULT_HASH_MISSING",
    "ORH_DONOR_DOMINANCE_CONTRACT_ONLY_EVIDENCE",
    "ORH_DONOR_DOMINANCE_MGA_SECURITY_MISSING",
    "ORH_DONOR_DOMINANCE_NON_REPRODUCIBLE_METADATA",
    "ORH_DONOR_DOMINANCE_BENCHMARK_CLEAN_OVERCLAIM",
    "ORH_DONOR_DOMINANCE_OVERCLAIM",
    "ORH_LARGE_SCALE_BENCHMARK_TIER_UNAVAILABLE",
}
REQUIRED_MISSING_LANE_DIAGNOSTICS = {
    "ORH_DONOR_DOMINANCE_LIVE_ROUTE_MISSING.EMBEDDED",
    "ORH_DONOR_DOMINANCE_LIVE_ROUTE_MISSING.IPC",
    "ORH_DONOR_DOMINANCE_LIVE_ROUTE_MISSING.INET",
    "ORH_DONOR_DOMINANCE_DONOR_CONTROLS_MISSING",
    "ORH_LARGE_SCALE_BENCHMARK_TIER_UNAVAILABLE",
}


def fail(message: str) -> None:
    print(f"ORH-270 gate failure: {message}", file=sys.stderr)
    raise SystemExit(1)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def load_json(path: Path) -> dict:
    try:
        with path.open("r", encoding="utf-8") as handle:
            return json.load(handle)
    except Exception as exc:  # pragma: no cover - diagnostic path
        fail(f"unable to load {path}: {exc}")


def validate_blocker_artifact(artifact: dict) -> None:
    require(artifact.get("row_id") == "ORH-270", "row id mismatch")
    require(artifact.get("gate_id") == "ORH-GATE-270", "gate id mismatch")
    require(
        artifact.get("status") == "completed-blocked",
        "ORH-270 must be completed-blocked without fresh live dominance proof",
    )
    require(
        artifact.get("diagnostic_code") == "ORH_DONOR_DOMINANCE_NOT_ACHIEVED",
        "top-level blocker diagnostic mismatch",
    )
    require(artifact.get("benchmark_clean") is False, "benchmark-clean overclaim")
    require(
        artifact.get("donor_dominance_claim") is False,
        "donor-dominance claim must be false",
    )
    require(
        artifact.get("performance_superiority_claim") is False,
        "performance superiority claim must be false",
    )
    require(
        artifact.get("fresh_live_dominance_proof_present") is False,
        "fresh live dominance proof is incorrectly marked present",
    )
    require(
        REQUIRED_ROUTES.issubset(set(artifact.get("required_scratchbird_routes", []))),
        "required embedded/ipc/inet route list incomplete",
    )
    require(
        REQUIRED_DONORS.issubset(set(artifact.get("required_donor_engines", []))),
        "required donor engine list incomplete",
    )
    require(
        artifact.get("required_comparable_workloads"),
        "comparable workload list missing",
    )

    available = artifact.get("available_artifacts", [])
    require(available, "available artifact classification missing")
    for entry in available:
        require(
            entry.get("fresh_live_dominance_authority") is False,
            f"{entry.get('path')} incorrectly marked as dominance authority",
        )

    missing_lanes = artifact.get("missing_lanes", [])
    require(missing_lanes, "missing lane diagnostics absent")
    lane_codes = {lane.get("diagnostic_code") for lane in missing_lanes}
    require(
        REQUIRED_MISSING_LANE_DIAGNOSTICS.issubset(lane_codes),
        "missing lane diagnostic coverage incomplete",
    )
    for lane in missing_lanes:
        require(lane.get("lane_id"), "missing lane id")
        require(lane.get("route_label"), "missing route label")
        require(lane.get("scale_tier"), "missing scale tier")
        require(lane.get("cache_phase"), "missing cache phase")
        require(lane.get("exact_blocker"), "missing exact blocker text")
        require(
            lane.get("p50_ms") is None
            and lane.get("p95_ms") is None
            and lane.get("p99_ms") is None,
            "blocked lane must not fabricate percentile timings",
        )
        require(
            lane.get("result_hash") is None,
            "blocked lane must not fabricate result hash",
        )

    negative_codes = {
        case.get("diagnostic_code")
        for case in artifact.get("negative_controls", [])
        if case.get("validated") is True
    }
    require(
        REQUIRED_NEGATIVE_DIAGNOSTICS.issubset(negative_codes),
        "negative control diagnostic coverage incomplete",
    )

    decision = artifact.get("closure_decision", {})
    require(
        decision.get("selected_status") == "completed-blocked",
        "closure decision status mismatch",
    )
    require(
        decision.get("completion_allowed") is False,
        "completion must not be allowed without fresh dominance proof",
    )
    require(
        decision.get("completed_blocked_allowed") is True,
        "completed-blocked decision not explicitly allowed",
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--artifact", required=True, type=Path)
    args = parser.parse_args()
    validate_blocker_artifact(load_json(args.artifact))
    print("ORH-270 donor-dominance closure gate passed: completed-blocked")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
