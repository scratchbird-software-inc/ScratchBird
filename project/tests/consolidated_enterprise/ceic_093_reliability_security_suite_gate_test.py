#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""CTest wrapper for CEIC-093 reliability/security validation.

SEARCH_KEY: CEIC_093_RELIABILITY_SECURITY_SUITE_GATE_TEST
"""

from __future__ import annotations

import argparse
import copy
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
from collections.abc import Callable
from typing import Any


def run(command: list[str], *, expect_success: bool) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, text=True, capture_output=True, check=False)
    if expect_success and result.returncode != 0:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise AssertionError(f"expected success: {' '.join(command)}")
    if not expect_success and result.returncode == 0:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise AssertionError(f"expected failure: {' '.join(command)}")
    return result


def expect_failure_contains(command: list[str], text: str) -> None:
    result = run(command, expect_success=False)
    output = result.stdout + result.stderr
    if text not in output:
        raise AssertionError(f"expected failure output to contain {text!r}, got: {output}")


def load_tool(tool_path: pathlib.Path):
    spec = importlib.util.spec_from_file_location("ceic_reliability_security_suite", tool_path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot load tool module: {tool_path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def write_manifest(path: pathlib.Path, model: dict[str, Any]) -> None:
    path.write_text(json.dumps(model, indent=2, sort_keys=True), encoding="utf-8")


def first_lane(model: dict[str, Any], lane_class: str | None = None) -> dict[str, Any]:
    for lane in model["lanes"]:
        if lane_class is None or lane["lane_class"] == lane_class:
            return lane
    raise AssertionError(f"missing lane class: {lane_class}")


def first_security_check(model: dict[str, Any], check_id: str | None = None) -> dict[str, Any]:
    lane = first_lane(model, "security_negative")
    for check in lane["security_negative_checks"]:
        if check_id is None or check["check_id"] == check_id:
            return check
    raise AssertionError(f"missing security check: {check_id}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()

    repo_root = pathlib.Path(args.repo_root).resolve()
    tool = repo_root / "project/tools/ceic_reliability_security_suite.py"
    command = [
        sys.executable,
        str(tool),
        "--repo-root",
        str(repo_root),
        "--skip-execution_plan-control",
    ]

    positive = run(command, expect_success=True)
    module = load_tool(tool)
    base_model = module.default_model(repo_root)

    with tempfile.TemporaryDirectory(prefix="ceic_093_reliability_security_") as temp_text:
        temp_dir = pathlib.Path(temp_text)
        cases: list[tuple[str, str, Callable[[dict[str, Any]], None]]] = [
            (
                "missing_lane_class",
                "missing_lane_class",
                lambda model: model["lanes"].remove(first_lane(model, "soak_72h")),
            ),
            (
                "pending_or_defined_only_lane",
                "pending_or_defined_only_lane",
                lambda model: first_lane(model, "soak_24h").update({"status": "pending"}),
            ),
            (
                "defined_only_execution_mode",
                "pending_or_defined_only_lane",
                lambda model: first_lane(model, "soak_24h").update({"execution_mode": "defined_only"}),
            ),
            (
                "missing_soak_duration",
                "missing_soak_duration",
                lambda model: first_lane(model, "soak_72h").update({"duration_hours": 12}),
            ),
            (
                "missing_7d_soak_duration",
                "missing_soak_duration",
                lambda model: first_lane(model, "soak_7d").update({"duration_hours": 72}),
            ),
            (
                "missing_high_concurrency",
                "missing_high_concurrency",
                lambda model: first_lane(model, "high_concurrency").update({"worker_counts": [8, 32, 64]}),
            ),
            (
                "missing_crash_fault",
                "missing_crash_fault",
                lambda model: first_lane(model, "crash_fault_injection")["fault_points"].remove(
                    "optimizer_stats_crash_reopen"
                ),
            ),
            (
                "missing_sanitizer_static",
                "missing_sanitizer_static",
                lambda model: first_lane(model, "sanitizer_static")["tool_kinds"].remove("tsan"),
            ),
            (
                "missing_memory_pressure",
                "missing_memory_pressure",
                lambda model: first_lane(model, "memory_pressure")["workloads"].remove("spill_storm"),
            ),
            (
                "bypassed_security_negative",
                "bypassed_security_negative",
                lambda model: first_security_check(model, "authorization_bypass").update({"bypassed": True}),
            ),
            (
                "security_negative_removed",
                "bypassed_security_negative",
                lambda model: first_lane(model, "security_negative")["security_negative_checks"].pop(),
            ),
            (
                "missing_artifact_retention",
                "missing_artifact_retention",
                lambda model: first_lane(model, "soak_24h")["artifact_retention"].update(
                    {"failure_inventory_retained": False}
                ),
            ),
            (
                "synthetic_evidence",
                "synthetic_evidence",
                lambda model: first_lane(model, "soak_24h").update({"synthetic_evidence": True}),
            ),
            (
                "fixture_test_only",
                "fixture_test_only",
                lambda model: first_lane(model, "soak_24h").update({"fixture_or_test_only_evidence": True}),
            ),
            (
                "local_cluster_claim",
                "local_cluster_claim",
                lambda model: first_lane(model, "soak_24h").update({"local_cluster_production_claim": True}),
            ),
            (
                "unsafe_authority",
                "unsafe_authority",
                lambda model: first_lane(model, "security_negative")["authority_flags"].update(
                    {"security_authority": True}
                ),
            ),
            (
                "successor_overclaim",
                "successor_overclaim",
                lambda model: model.update({"ceic_094_scale_claimed": True}),
            ),
            (
                "missing_predecessor_coupling",
                "missing_predecessor_coupling",
                lambda model: model["predecessor_coupling"].update({"route_chain_proof_consumed": False}),
            ),
            (
                "missing_route_family",
                "missing_route_family",
                lambda model: first_lane(model, "soak_24h")["route_families"].remove("graph_seed_traversal"),
            ),
        ]

        for name, expected, mutate in cases:
            model = copy.deepcopy(base_model)
            mutate(model)
            path = temp_dir / f"{name}.json"
            write_manifest(path, model)
            expect_failure_contains(command + ["--manifest", str(path)], expected)

    print("ceic_093_reliability_security_suite_gate_test=pass")
    print(positive.stdout.strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
