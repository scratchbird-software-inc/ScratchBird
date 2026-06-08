#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""CTest wrapper for CEIC-092 route-chain validation.

SEARCH_KEY: CEIC_092_ROUTE_CHAIN_GATE_TEST
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
    spec = importlib.util.spec_from_file_location("ceic_route_chain_proof", tool_path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot load tool module: {tool_path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def write_manifest(path: pathlib.Path, model: dict[str, Any]) -> None:
    path.write_text(json.dumps(model, indent=2, sort_keys=True), encoding="utf-8")


def first_route(model: dict[str, Any], family: str | None = None) -> dict[str, Any]:
    for route in model["routes"]:
        if family is None or route["route_family"] == family:
            return route
    raise AssertionError(f"missing route family: {family}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()

    repo_root = pathlib.Path(args.repo_root).resolve()
    tool = repo_root / "project/tools/ceic_route_chain_proof.py"
    command = [sys.executable, str(tool), "--repo-root", str(repo_root)]

    positive = run(command, expect_success=True)
    module = load_tool(tool)
    base_model = module.default_model(repo_root)

    with tempfile.TemporaryDirectory(prefix="ceic_092_route_chain_") as temp_text:
        temp_dir = pathlib.Path(temp_text)
        cases: list[tuple[str, str, Callable[[dict[str, Any]], None]]] = [
            (
                "missing_memory_reservation",
                "missing_memory_reservation",
                lambda model: first_route(model).pop("memory_reservation"),
            ),
            (
                "missing_optimizer_readiness",
                "missing_optimizer_readiness",
                lambda model: first_route(model)["optimizer_plan_admission"].update({"plan_admitted": False}),
            ),
            (
                "missing_index_recheck",
                "missing_index_recheck",
                lambda model: first_route(model, "vector_ann_topk")["index_access"].update(
                    {"exact_rerank_performed": False}
                ),
            ),
            (
                "missing_mga_security_recheck",
                "missing_mga_security_recheck",
                lambda model: first_route(model)["mga_security_recheck"].update(
                    {"security_authorization_rechecked": False}
                ),
            ),
            (
                "result_mismatch",
                "result_mismatch",
                lambda model: first_route(model)["result_equivalence"].update(
                    {"route_result_hash": module.digest("different-route-result")}
                ),
            ),
            (
                "agent_authority_claim",
                "agent_authority_claim",
                lambda model: first_route(model)["agent_recommendation"].update({"action_executed": True}),
            ),
            (
                "placeholder_evidence",
                "placeholder_evidence",
                lambda model: first_route(model)["result_equivalence"].update(
                    {"result_contract": "result-contract-v1"}
                ),
            ),
            (
                "local_cluster_claim",
                "local_cluster_claim",
                lambda model: first_route(model).update({"local_cluster_production_claim": True}),
            ),
            (
                "synthetic_evidence",
                "synthetic_evidence",
                lambda model: first_route(model).update({"synthetic_evidence": True}),
            ),
            (
                "fixture_test_only",
                "fixture_test_only",
                lambda model: first_route(model).update({"fixture_or_test_only_evidence": True}),
            ),
            (
                "missing_ceic090_coupling",
                "missing_ceic090_coupling",
                lambda model: first_route(model)["ceic_090_metrics_evidence"]["metric_families"].remove(
                    "support_bundle_generation"
                ),
            ),
            (
                "missing_ceic091_coupling",
                "missing_ceic091_coupling",
                lambda model: first_route(model)["ceic_091_support_bundle_evidence"]["sections"].remove("agent"),
            ),
            (
                "successor_overclaim",
                "successor_overclaim",
                lambda model: model.update({"ceic_093_soak_claimed": True}),
            ),
            (
                "missing_route_family",
                "missing_route_family",
                lambda model: model["routes"].pop(),
            ),
        ]

        for name, expected, mutate in cases:
            model = copy.deepcopy(base_model)
            mutate(model)
            path = temp_dir / f"{name}.json"
            write_manifest(path, model)
            expect_failure_contains(command + ["--manifest", str(path), "--skip-execution_plan-control"], expected)

    print("ceic_092_route_chain_gate_test=pass")
    print(positive.stdout.strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
