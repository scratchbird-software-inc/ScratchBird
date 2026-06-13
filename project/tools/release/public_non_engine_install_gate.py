#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate optional non-engine install packaging for manager/runtime tools."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from typing import Any


FORBIDDEN_REFERENCE_FRAGMENTS = (
    "docs" + "/" + "execution-plans",
    "docs" + "/" + "completed-execution-plans",
    "docs" + "/" + "findings",
    "." + "git",
    "/" + "home" + "/" + "dcalford",
    "ScratchBird" + "-Private",
)

REQUIRED_NON_ENGINE_FILES = (
    "bin/SBmgr",
)

MANAGER_INSTALL_CONTRACT = "bin/sbmn_manager"

FORBIDDEN_COMPONENT_ONLY_ENGINE_FILES = (
    "lib/libSBcore.so",
    "bin/SBcore.dll",
    "lib/SBcore.dll",
    "lib/cmake/ScratchBirdEngine/ScratchBirdEngineConfig.cmake",
    "lib/cmake/ScratchBirdEngine/ScratchBirdEngineTargets.cmake",
)


def fail(message: str) -> None:
    print(f"public_non_engine_install_gate=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def rel(path: Path, root: Path) -> str:
    return path.relative_to(root).as_posix()


def reject_private_reference(value: str, context: str) -> None:
    if Path(value).is_absolute():
        fail(f"absolute_path_recorded:{context}:{value}")
    for fragment in FORBIDDEN_REFERENCE_FRAGMENTS:
        if fragment in value:
            fail(f"private_reference_recorded:{context}:{value}")


def run(command: list[str], *, cwd: Path) -> str:
    result = subprocess.run(
        command,
        cwd=str(cwd),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode != 0:
        print(result.stdout)
        fail(f"command_failed:{command[0]}:exit={result.returncode}")
    return result.stdout


def safe_rmtree(path: Path, work_root: Path) -> None:
    resolved = path.resolve()
    root = work_root.resolve()
    require(resolved == root or root in resolved.parents, f"refusing_to_remove:{resolved}")
    if resolved.exists():
        shutil.rmtree(resolved)


def require_file(path: Path, root: Path, label: str) -> str:
    require(path.exists() and path.is_file(), f"missing_{label}:{rel(path, root)}")
    return rel(path, root)


def configure_build_install(args: argparse.Namespace, work_root: Path) -> tuple[Path, Path]:
    nested_build = work_root / "nested-build"
    prefix = work_root / "install"
    configure_command = [
        str(args.cmake),
        "-S",
        str(args.project_root),
        "-B",
        str(nested_build),
        "-DSB_INSTALL_NON_ENGINE_COMPONENTS=ON",
        "-DSB_BUILD_DATABASE_LIFECYCLE_TESTS=OFF",
        "-DSB_BUILD_PUBLIC_RELEASE_CORRECTNESS=OFF",
        "-DSB_BUILD_TESTS=OFF",
        "-DSB_BUILD_SBMN_MANAGER=ON",
        "-DSB_BUILD_DRIVERS=OFF",
        "-DSB_BUILD_PARSERS=OFF",
        "-DSB_BUILD_UDR=OFF",
        "-DSCRATCHBIRD_ENABLE_DEBUG_LOGS=OFF",
        "-DSCRATCHBIRD_ENABLE_HOTPATH_TRACE=OFF",
        "-DSCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE=OFF",
        "-DSCRATCHBIRD_ENABLE_PREPARED_TRACE=OFF",
    ]
    for name in (
        "SB_LLVM_PROJECT_ROOT",
        "SB_LLVM_TOOLS_ROOT",
        "SB_LLVM_LIBRARY",
        "SB_LLVM_LINK_MODE",
    ):
        value = os.environ.get(name, "")
        if value:
            configure_command.append(f"-D{name}={value}")
    if os.name != "nt":
        configure_command.append("-DCMAKE_BUILD_TYPE=Release")
    run(configure_command, cwd=args.project_root)
    for target in (
        "sbmn_manager",
        "sb_isql",
        "sb_admin",
        "sb_backup",
        "sb_security",
        "sb_verify",
        "sbdriver_conformance",
    ):
        run([str(args.cmake), "--build", str(nested_build), "--target", target],
            cwd=args.project_root)
    run([
        str(args.cmake),
        "--install",
        str(nested_build),
        "--prefix",
        str(prefix),
        "--component",
        "non_engine_runtime",
    ], cwd=args.project_root)
    return nested_build, prefix


def validate_install(prefix: Path) -> dict[str, Any]:
    required_files: list[str] = []
    for required in REQUIRED_NON_ENGINE_FILES:
        path = prefix / required
        if sys.platform == "win32" and path.suffix != ".exe":
            path = path.with_suffix(".exe")
        required_files.append(require_file(path, prefix, required))
        require(path.stat().st_size > 0, f"empty_required_file:{rel(path, prefix)}")
    for forbidden in FORBIDDEN_COMPONENT_ONLY_ENGINE_FILES:
        require(not (prefix / forbidden).exists(),
                f"unexpected_engine_component_artifact:{forbidden}")
    return {
        "installed_non_engine_files": sorted(required_files),
        "engine_component_artifacts_absent": sorted(FORBIDDEN_COMPONENT_ONLY_ENGINE_FILES),
    }


def build_evidence(args: argparse.Namespace) -> dict[str, Any]:
    repo_root = args.repo_root.resolve()
    project_root = args.project_root.resolve()
    build_root = args.build_root.resolve()
    work_root = args.work_root.resolve()
    output = args.output.resolve()
    require(repo_root.is_dir() and project_root.is_dir() and build_root.is_dir(),
            "input_root_missing")
    require(project_root == repo_root / "project", "project_root_mismatch")
    try:
        work_record = work_root.relative_to(build_root).as_posix()
    except ValueError:
        fail("work_root_must_be_under_build_root")
    reject_private_reference(work_record, "work_root")
    try:
        output_record = output.relative_to(build_root).as_posix()
    except ValueError:
        fail("output_must_be_under_build_root")
    reject_private_reference(output_record, "output")

    safe_rmtree(work_root, work_root)
    work_root.mkdir(parents=True, exist_ok=True)
    nested_build, prefix = configure_build_install(args, work_root)
    install_evidence = validate_install(prefix)

    return {
        "schema_version": 1,
        "gate": "PCR-GATE-140",
        "policy": {
            "public_tree_inputs_only": True,
            "private_docs_required": False,
            "git_history_required": False,
            "optional_non_engine_install": True,
            "non_engine_component": "non_engine_runtime",
            "engine_package_export_boundary_proven_by": "public_install_consumer_gate",
        },
        "configure": {
            "build_dir": rel(nested_build, build_root),
            "install_prefix": rel(prefix, build_root),
            "SB_INSTALL_NON_ENGINE_COMPONENTS": "ON",
            "SB_BUILD_SBMN_MANAGER": "ON",
            "install_component": "non_engine_runtime",
        },
        "install": install_evidence,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--work-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cmake", type=Path, required=True)
    args = parser.parse_args()

    evidence = build_evidence(args)
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n",
                      encoding="utf-8")
    print(f"public_non_engine_install_output={output.relative_to(args.build_root.resolve()).as_posix()}")
    print("public_non_engine_install_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
