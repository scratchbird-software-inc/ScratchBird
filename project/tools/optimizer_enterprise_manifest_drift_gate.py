#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""OEIC optimizer source and implementation-anchor drift gate.

SEARCH_KEY: OEIC_OPTIMIZER_GENERATED_MANIFEST_DRIFT_GATE
"""

from __future__ import annotations

import argparse
import pathlib
import sys


REQUIRED_OPTIMIZER_TEST_TOKENS = {
    "optimizer_enterprise_inventory_gate",
    "optimizer_enterprise_metric_manifest_gate",
    "optimizer_enterprise_route_validation_gate",
    "optimizer_enterprise_maintainability_gate",
    "optimizer_enterprise_manifest_drift_gate",
    "OEIC-080",
    "OEIC-085",
    "OEIC-090",
    "OEIC-091",
}

REQUIRED_PROJECT_TEST_TOKENS = {
    "optimizer_production_build_cmake_gate",
    "optimizer_production_build_configure_gate",
}

REQUIRED_SOURCE_ANCHORS = {
    "project/src/engine/optimizer/runtime_consumption_benchmark_evidence.cpp": {
        "OEIC_OPTIMIZER_MAINTAINABILITY_REFACTOR",
        "RuntimeEvidenceIsCleanlyConsumed",
        "ValidateOptimizerBenchmarkRouteEvidence",
        "ValidateReferenceDominanceTarget",
    },
    "project/tests/optimizer/optimizer_enterprise_route_validation_gate.cpp": {
        "OEIC_BENCHMARK_CLEAN_OPTIMIZER_ROUTE",
        "OEIC_SCALE_REFERENCE_COMPARISON_SUITE",
        "OEIC_LIVE_PARSER_SBLR_OPTIMIZER_EXECUTOR_ROUTE",
        "OEIC_OPTIMIZER_CRASH_RESTART_FEEDBACK",
        "OEIC_OPTIMIZER_SECURITY_NEGATIVE_SUITE",
    },
    "project/tests/optimizer/optimizer_enterprise_maintainability_gate.cpp": {
        "OEIC_OPTIMIZER_MAINTAINABILITY_REFACTOR",
        "runtime_consumption_benchmark_evidence.cpp",
        "runtime_consumption_evidence.cpp",
    },
    "project/src/engine/optimizer/join_planner_full.cpp": {
        "OEIC_ENTERPRISE_JOIN_MEMO_FRONTIER",
    },
}


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def read(path: pathlib.Path) -> str:
    if not path.exists():
        fail(f"required source file missing: {path}")
    return path.read_text(errors="replace")


def require_tokens(path: pathlib.Path, tokens: set[str]) -> None:
    text = read(path)
    for token in sorted(tokens):
        if token not in text:
            fail(f"{path}: missing {token}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True)
    args = parser.parse_args()

    repo = pathlib.Path(args.repo).resolve()
    project_root = repo / "project" if (repo / "project").is_dir() else repo

    require_tokens(
        project_root / "tests" / "optimizer" / "CMakeLists.txt",
        REQUIRED_OPTIMIZER_TEST_TOKENS,
    )
    require_tokens(project_root / "CMakeLists.txt", REQUIRED_PROJECT_TEST_TOKENS)
    require_tokens(
        project_root / "src" / "engine" / "optimizer" / "CMakeLists.txt",
        {"runtime_consumption_benchmark_evidence.cpp"},
    )

    for rel_path, tokens in REQUIRED_SOURCE_ANCHORS.items():
        require_tokens(repo / rel_path, tokens)

    print("OEIC optimizer source manifest drift gate passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
