#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""CTest wrapper for CEIC-094 cross-platform release validation.

SEARCH_KEY: CEIC_094_CROSS_PLATFORM_RELEASE_GATE_TEST
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
    spec = importlib.util.spec_from_file_location("ceic_cross_platform_release_proof", tool_path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot load tool module: {tool_path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def write_manifest(path: pathlib.Path, model: dict[str, Any]) -> None:
    path.write_text(json.dumps(model, indent=2, sort_keys=True), encoding="utf-8")


def first_platform(model: dict[str, Any], family: str | None = None) -> dict[str, Any]:
    for row in model["platforms"]:
        if family is None or row["platform_family"] == family:
            return row
    raise AssertionError(f"missing platform family: {family}")


def first_area(model: dict[str, Any], family: str, area: str) -> dict[str, Any]:
    platform = first_platform(model, family)
    for row in platform["validation_areas"]:
        if row["area"] == area:
            return row
    raise AssertionError(f"missing area {area!r} for {family!r}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()

    repo_root = pathlib.Path(args.repo_root).resolve()
    tool = repo_root / "project/tools/ceic_cross_platform_release_proof.py"
    command = [sys.executable, str(tool), "--repo-root", str(repo_root)]

    positive = run(command, expect_success=True)
    module = load_tool(tool)
    base_model = module.default_model(repo_root)

    with tempfile.TemporaryDirectory(prefix="ceic_094_cross_platform_") as temp_text:
        temp_dir = pathlib.Path(temp_text)
        cases: list[tuple[str, str, Callable[[dict[str, Any]], None]]] = [
            (
                "missing_platform",
                "missing_platform",
                lambda model: model["platforms"].remove(first_platform(model, "bsd")),
            ),
            (
                "unsupported_claimed_platform",
                "unsupported_claimed_platform",
                lambda model: first_platform(model, "windows").update({"support_state": "unsupported_unclaimed"}),
            ),
            (
                "pending_platform",
                "pending_or_defined_only_platform",
                lambda model: first_platform(model, "linux").update({"status": "pending"}),
            ),
            (
                "fixture_test_only",
                "fixture_test_only",
                lambda model: first_platform(model, "linux").update({"fixture_or_test_only_evidence": True}),
            ),
            (
                "synthetic_evidence",
                "synthetic_evidence",
                lambda model: first_platform(model, "linux").update({"synthetic_evidence": True}),
            ),
            (
                "local_cluster_claim",
                "local_cluster_claim",
                lambda model: first_platform(model, "linux").update({"local_cluster_production_claim": True}),
            ),
            (
                "missing_production_gate",
                "production_build_gate",
                lambda model: first_platform(model, "linux").update({"production_gate_matrix_passed": False}),
            ),
            (
                "missing_signing_sbom",
                "missing_signing_sbom",
                lambda model: first_platform(model, "linux").update({"sbom_present": False}),
            ),
            (
                "missing_entropy_provider",
                "missing_entropy_provider",
                lambda model: first_area(model, "windows", "entropy_provider")["details"].update(
                    {"entropy_provider": "getrandom"}
                ),
            ),
            (
                "missing_validation_area",
                "missing_validation_area",
                lambda model: first_platform(model, "macos")["validation_areas"].pop(),
            ),
            (
                "missing_llvm_linkage",
                "missing_llvm_linkage",
                lambda model: first_platform(model, "linux").update({"llvm_static_option_validated": False}),
            ),
            (
                "unsafe_authority",
                "unsafe_authority",
                lambda model: first_platform(model, "linux")["authority_flags"].update(
                    {"cluster_authority": True}
                ),
            ),
            (
                "successor_overclaim",
                "successor_overclaim",
                lambda model: model.update({"ceic_095_enterprise_readiness_claimed": True}),
            ),
            (
                "missing_predecessor_coupling",
                "missing_predecessor_coupling",
                lambda model: model["predecessor_coupling"].update(
                    {"ceic_093_reliability_security_consumed": False}
                ),
            ),
        ]

        for name, expected, mutate in cases:
            model = copy.deepcopy(base_model)
            mutate(model)
            path = temp_dir / f"{name}.json"
            write_manifest(path, model)
            expect_failure_contains(command + ["--manifest", str(path), "--skip-execution_plan-control"], expected)

    print("ceic_094_cross_platform_release_gate_test=pass")
    print(positive.stdout.strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
