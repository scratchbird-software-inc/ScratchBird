#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""DBLC-013A manager-family lifecycle conformance gate."""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys


def read_text(path: pathlib.Path) -> str:
    return path.read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def run_gate(path: pathlib.Path, timeout: int, extra_args: list[str] | None = None) -> None:
    require(path.exists(), f"required gate executable missing: {path}")
    command = [str(path)]
    if extra_args:
        command.extend(extra_args)
    result = subprocess.run(
        command,
        cwd=str(path.parent),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
        check=False,
    )
    if result.returncode != 0:
        raise AssertionError(
            f"{path.name} failed with exit code {result.returncode}\n"
            f"stdout:\n{result.stdout}\n"
            f"stderr:\n{result.stderr}"
        )


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--build-root", required=True)
    parser.add_argument("--manager-exe", required=True)
    parser.add_argument("--protocol-unit", required=True)
    parser.add_argument("--runtime-integration", required=True)
    parser.add_argument("--fuzz-gate", required=True)
    parser.add_argument("--no-spin-gate", required=True)
    parser.add_argument("--no-spin-source", required=True)
    args = parser.parse_args(argv)

    repo = pathlib.Path(args.repo_root)
    private_root = repo.parent
    build = pathlib.Path(args.build_root)
    manager_exe = pathlib.Path(args.manager_exe)

    require(manager_exe.exists(), f"sbmn_manager executable missing: {manager_exe}")
    require(manager_exe.stem == "sbmn_manager", "manager lifecycle build must produce sbmn_manager")
    require(not any((manager_exe.parent / name).exists() for name in ("sbmc_manager", "sbmc_manager.exe")),
            "standalone lifecycle build must not produce sbmc_manager")
    require(build.exists(), "build root must exist")

    top_cmake = read_text(repo / "CMakeLists.txt")
    node_cmake = read_text(repo / "src" / "manager" / "node" / "CMakeLists.txt")
    require("add_executable(sbmc_manager" not in top_cmake, "top-level build must not define sbmc_manager")
    require("add_executable(sbmc_manager" not in node_cmake, "node manager build must not define sbmc_manager")

    sbmn_spec = read_text(private_root / "docs" / "contracts" / "implementation_inputs" / "sbmn_manager.md")
    require("No `sbmc_manager` target is produced by the standalone build." in sbmn_spec,
            "sbmn_manager packet must keep sbmc_manager private")
    require("MANAGER.CLUSTER_ONLY_FORBIDDEN" in sbmn_spec,
            "sbmn_manager packet must define cluster-only refusal diagnostic")

    runtime = read_text(repo / "src" / "manager" / "node" / "manager_runtime.cpp")
    runtime_test = read_text(repo / "tests" / "manager" / "runtime_integration_tests.cpp")
    require("MANAGER.CLUSTER_ONLY_FORBIDDEN" in runtime,
            "sbmn_manager runtime must reject cluster-only commands explicitly")
    require("manager.join_cluster" in runtime_test and "MANAGER.CLUSTER_ONLY_FORBIDDEN" in runtime_test,
            "runtime integration test must cover cluster-only command refusal")

    run_gate(pathlib.Path(args.protocol_unit), timeout=30)
    run_gate(pathlib.Path(args.runtime_integration), timeout=45)
    run_gate(pathlib.Path(args.fuzz_gate), timeout=30)
    run_gate(pathlib.Path(args.no_spin_gate), timeout=30, extra_args=[args.no_spin_source])

    print("database_lifecycle_manager_conformance=passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except AssertionError as exc:
        print(f"database_lifecycle_manager_conformance=failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
