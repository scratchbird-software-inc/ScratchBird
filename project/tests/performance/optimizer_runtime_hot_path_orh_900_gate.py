#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""ORH-900 focused live route benchmark closure validator.

This gate does not synthesize live benchmark results. It validates that the
focused live route closure is recorded only as completed-blocked unless fresh
embedded, IPC, and INET optimized-route benchmark evidence exists.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


REQUIRED_ROUTES = {"embedded", "ipc", "inet"}
REQUIRED_MISSING_ROUTE_DIAGNOSTICS = {
    "ORH_FOCUSED_LIVE_ROUTE_MISSING.EMBEDDED",
    "ORH_FOCUSED_LIVE_ROUTE_MISSING.IPC",
    "ORH_FOCUSED_LIVE_ROUTE_MISSING.INET",
}
REQUIRED_NEGATIVE_DIAGNOSTICS = {
    "ORH_FOCUSED_LIVE_ROUTE_STALE_ARTIFACT",
    "ORH_FOCUSED_LIVE_ROUTE_MISSING.EMBEDDED",
    "ORH_FOCUSED_LIVE_ROUTE_MISSING.IPC",
    "ORH_FOCUSED_LIVE_ROUTE_MISSING.INET",
    "ORH_FOCUSED_LIVE_ROUTE_LABEL_MISSING",
    "ORH_FOCUSED_LIVE_ROUTE_RESULT_HASH_MISSING",
    "ORH_FOCUSED_LIVE_ROUTE_MGA_SECURITY_MISSING",
    "ORH_FOCUSED_LIVE_ROUTE_CONTRACT_ONLY_EVIDENCE",
    "ORH_FOCUSED_LIVE_ROUTE_UNSAFE_AUTHORITY",
    "ORH_FOCUSED_LIVE_ROUTE_BENCHMARK_CLEAN_OVERCLAIM",
    "ORH_FOCUSED_LIVE_ROUTE_NO_RUNTIME_CONSUMPTION",
    "ORH_FOCUSED_LIVE_ROUTE_STALE_INDEX_REBASE_EVIDENCE",
}


def fail(message: str) -> None:
    print(f"ORH-900 gate failure: {message}", file=sys.stderr)
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
    require(artifact.get("row_id") == "ORH-900", "row id mismatch")
    require(artifact.get("gate_id") == "ORH-GATE-900", "gate id mismatch")
    require(
        artifact.get("status") == "completed-blocked",
        "ORH-900 must be completed-blocked without fresh live route proof",
    )
    require(
        artifact.get("diagnostic_code") == "ORH_FOCUSED_LIVE_ROUTE_BLOCKED",
        "top-level blocker diagnostic mismatch",
    )
    require(artifact.get("benchmark_clean") is False, "benchmark-clean overclaim")
    require(
        artifact.get("live_route_closure_claim") is False,
        "live route closure claim must be false",
    )
    require(
        artifact.get("performance_superiority_claim") is False,
        "performance superiority claim must be false",
    )
    require(
        artifact.get("fresh_live_embedded_ipc_inet_benchmark_proof_present")
        is False,
        "fresh embedded/ipc/inet proof is incorrectly marked present",
    )
    require(
        REQUIRED_ROUTES.issubset(set(artifact.get("required_routes", []))),
        "required embedded/ipc/inet route list incomplete",
    )

    fields = set(artifact.get("required_evidence_fields", []))
    for field in {
        "route_label",
        "lane_identity",
        "result_hash",
        "p50_ms",
        "p95_ms",
        "p99_ms",
        "live_execution",
        "runtime_consumed",
        "optimized_path_consumed",
        "mga_security_evidence",
        "parser_client_donor_authority",
        "benchmark_clean_overclaim",
    }:
        require(field in fields, f"required evidence field missing: {field}")

    available = artifact.get("available_artifacts", [])
    require(available, "available artifact classification missing")
    for entry in available:
        require(
            entry.get("fresh_live_route_closure_authority") is False,
            f"{entry.get('path')} incorrectly marked as live-route authority",
        )
        require(entry.get("reason"), "available artifact reason missing")

    missing_lanes = artifact.get("missing_lanes", [])
    require(missing_lanes, "missing lane diagnostics absent")
    route_codes = {lane.get("diagnostic_code") for lane in missing_lanes}
    require(
        REQUIRED_MISSING_ROUTE_DIAGNOSTICS.issubset(route_codes),
        "missing route diagnostic coverage incomplete",
    )
    for lane in missing_lanes:
        require(lane.get("lane_id"), "missing lane id")
        require(lane.get("route_label") in REQUIRED_ROUTES, "missing route label")
        require(lane.get("lane_identity"), "missing lane identity")
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
        require(
            lane.get("runtime_consumed") is False
            and lane.get("optimized_path_consumed") is False,
            "blocked lane must not claim runtime optimized consumption",
        )
        require(
            lane.get("mga_security_evidence") is False,
            "blocked lane must not fabricate MGA/security evidence",
        )
        require(
            lane.get("parser_client_donor_authority") is False,
            "blocked lane must reject parser/client/donor authority",
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
        "completion must not be allowed without fresh live route proof",
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
    print("ORH-900 focused live route closure gate passed: completed-blocked")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
