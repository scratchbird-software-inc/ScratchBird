#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""CTest wrapper for CEIC-095 final enterprise manifest validation.

SEARCH_KEY: CEIC_095_FINAL_ENTERPRISE_MANIFEST_GATE_TEST
"""

from __future__ import annotations

import argparse
import copy
import importlib.util
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
    spec = importlib.util.spec_from_file_location("ceic_final_enterprise_manifest", tool_path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot load tool module: {tool_path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def write_manifest(path: pathlib.Path, module: Any, model: dict[str, Any]) -> None:
    path.write_text(module.render_json(model), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()

    repo_root = pathlib.Path(args.repo_root).resolve()
    tool = repo_root / "project/tools/ceic_final_enterprise_manifest.py"
    command = [sys.executable, str(tool), "--repo-root", str(repo_root)]

    positive = run(command + ["--check-only"], expect_success=True)
    module = load_tool(tool)
    execution_plan_root = module.resolve_execution_plan(repo_root)
    base_model = module.build_manifest(repo_root, execution_plan_root)

    with tempfile.TemporaryDirectory(prefix="ceic_095_final_manifest_") as temp_text:
        temp_dir = pathlib.Path(temp_text)
        cases: list[tuple[str, str, Callable[[dict[str, Any]], None]]] = [
            (
                "tracker_pending",
                "tracker_status",
                lambda model: model["tracker_summary"]["status_counts"].update({"pending": 1}),
            ),
            (
                "gate_pending",
                "gate_status",
                lambda model: model["acceptance_gate_summary"]["status_counts"].update({"pending": 1}),
            ),
            (
                "artifact_planned",
                "artifact_status",
                lambda model: model["artifact_summary"]["status_counts"].update({"planned": 1}),
            ),
            (
                "manifest_missing",
                "missing_manifest_artifact",
                lambda model: model["artifact_summary"]["required_manifests"][0].update({"exists": False}),
            ),
            (
                "risk_open",
                "risk_status",
                lambda model: model["risk_summary"]["status_counts"].update({"open": 1}),
            ),
            (
                "dependency_pending",
                "dependency_status",
                lambda model: model["dependency_summary"]["status_counts"].update({"pending": 1}),
            ),
            (
                "trace_pending",
                "traceability_status",
                lambda model: model["traceability_summary"]["status_counts"].update({"pending": 1}),
            ),
            (
                "audit_pending",
                "audit_status",
                lambda model: model["implementation_audit_summary"]["status_counts"].update({"pending": 1}),
            ),
            (
                "local_cluster_claim",
                "local_cluster_claim",
                lambda model: model["cluster_boundary"].update({"cluster_production_claim": "enabled"}),
            ),
            (
                "static_only_claim",
                "static_only_claim",
                lambda model: model["static_claim_boundary"].update({"static_only_claims_allowed": True}),
            ),
            (
                "unsafe_authority",
                "unsafe_authority",
                lambda model: model["authority_boundary"].update({"transaction_finality_authority": True}),
            ),
            (
                "successor_overclaim",
                "successor_overclaim",
                lambda model: model["successor_boundary"].update(
                    {"ceic_successor_overclaim": True, "successor_slice_ids_claimed": ["CEIC-096"]}
                ),
            ),
            (
                "cmake_missing",
                "cmake_registration",
                lambda model: model["cmake_inclusion"]["CEIC-094"].update({"target_present": False}),
            ),
        ]

        for name, expected, mutate in cases:
            model = copy.deepcopy(base_model)
            mutate(model)
            path = temp_dir / f"{name}.json"
            write_manifest(path, module, model)
            expect_failure_contains(command + ["--manifest", str(path), "--skip-execution_plan-control"], expected)

    print("ceic_095_final_enterprise_manifest_gate_test=pass")
    print(positive.stdout.strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
