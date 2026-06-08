#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""MMCH-081 production/test build separation gate.

This is a source/build-metadata gate: it proves that memory probes, deterministic
fault hooks, and test-only temp-workspace paths are visibly guarded and that
production runtime code keeps those paths behind explicit test/probe build
surfaces.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import sys


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--build-metadata", required=True)
    args = parser.parse_args()

    repo_root = Path(args.repo_root)
    project_root = repo_root / "project"
    memory_hpp = read(project_root / "src/core/memory/memory.hpp")
    memory_cpp = read(project_root / "src/core/memory/memory.cpp")
    temp_cpp = read(project_root / "src/core/memory/temp_workspace_lifecycle.cpp")
    root_cmake = read(project_root / "CMakeLists.txt")
    db_cmake = read(project_root / "tests/database_lifecycle/CMakeLists.txt")
    optimizer_cmake = read(project_root / "tests/optimizer/CMakeLists.txt")
    metadata = read(Path(args.build_metadata))

    require("MMCH_MEMORY_PRODUCTION_TEST_SEPARATION" in db_cmake,
            "CMake gate must carry MMCH_MEMORY_PRODUCTION_TEST_SEPARATION")
    require("SCRATCHBIRD_MEMORY_FAILURE_INJECTION_TEST_GUARD" in memory_hpp,
            "allocation failure injection must require compile-time test guard")
    require("compile_time_test_guard_missing" in memory_cpp,
            "production allocation failure injection must fail without test guard")
    require("fixture_enabled" in memory_cpp and "fixture_name" in memory_cpp,
            "fault injection must require explicit fixture identity")
    require("memory_allocation_failure_injection_gate" in optimizer_cmake,
            "fault injection must be confined to a named test gate")
    require("SCRATCHBIRD_MEMORY_FAILURE_INJECTION_TEST_GUARD=1" in optimizer_cmake,
            "test guard macro must be target-local to the test executable")
    require("SB_BUILD_MEMORY_PROBE" in root_cmake and 'OFF' in root_cmake,
            "memory probe must remain opt-in, not production default")
    require("MMCH_SECURE_TEMP_WORKSPACE" in temp_cpp,
            "temp workspace production path must carry secure creation anchor")
    require("CREATE_NEW" in temp_cpp or "O_EXCL" in temp_cpp,
            "temp workspace must use exclusive-create semantics")
    require("O_NOFOLLOW" in temp_cpp or "reparse" in temp_cpp.lower(),
            "temp workspace must include symlink/reparse refusal semantics")

    for forbidden in (
        "SCRATCHBIRD_MEMORY_FAILURE_INJECTION_TEST_GUARD=ON",
        "SCRATCHBIRD_MEMORY_FAILURE_INJECTION_TEST_GUARD:BOOL=ON",
        "SB_BUILD_MEMORY_PROBE=ON",
    ):
        require(forbidden not in metadata,
                f"configured production metadata enables test-only memory path: {forbidden}")

    print("MMCH_MEMORY_PRODUCTION_TEST_SEPARATION: PASS")
    print("memory_test_hooks_target_local=true")
    print("memory_probe_opt_in=true")
    print("memory_temp_workspace_test_paths_guarded=true")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"MMCH_MEMORY_PRODUCTION_TEST_SEPARATION: FAIL: {exc}",
              file=sys.stderr)
        raise SystemExit(1)
