#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Build and run an external consumer from a staged public install."""

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

REQUIRED_INSTALL_FILES = (
    "include/scratchbird/engine/engine.h",
    "include/scratchbird/engine/version.h",
    "include/scratchbird/engine/export.h",
    "include/scratchbird/engine/sblr_envelope.hpp",
    "lib/cmake/ScratchBirdEngine/ScratchBirdEngineConfig.cmake",
    "lib/cmake/ScratchBirdEngine/ScratchBirdEngineTargets.cmake",
)

FORBIDDEN_ENGINE_ONLY_ARTIFACTS = (
    "bin/SBsrv",
    "bin/SBgate",
    "bin/sbmn_manager",
    "bin/SBmgr",
    "bin/SBParser",
    "bin/SB_FBSQL_Parser",
    "bin/sb_ipc_tester",
    "lib/libSBParser_udr.a",
    "lib/libSB_FBSQL_Parser_udr.a",
    "share/scratchbird/drivers/DriverPackageManifest.csv",
    "share/scratchbird/examples/core_beta_qa/manifest.json",
)


def fail(message: str) -> None:
    print(f"public_install_consumer_gate=fail:{message}", file=sys.stderr)
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


def run(command: list[str], *, cwd: Path, env: dict[str, str] | None = None) -> str:
    result = subprocess.run(
        command,
        cwd=str(cwd),
        env=env,
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


def installed_engine_library(prefix: Path) -> str:
    candidates = (
        prefix / "lib" / "libSBcore.so",
        prefix / "bin" / "SBcore.dll",
        prefix / "lib" / "libSBcore.dll.a",
        prefix / "lib" / "SBcore.dll",
    )
    for candidate in candidates:
        if candidate.exists() and candidate.is_file():
            return rel(candidate, prefix)
    fail("missing_engine_shared_library")


def scan_text_file(path: Path, root: Path) -> None:
    path_text = rel(path, root)
    reject_private_reference(path_text, "scan_path")
    text = path.read_text(encoding="utf-8")
    for fragment in FORBIDDEN_REFERENCE_FRAGMENTS:
        if fragment in text:
            fail(f"private_reference_in_file:{path_text}")


def validate_example_source(project_root: Path) -> list[str]:
    example_root = project_root / "examples" / "public_engine_consumer_smoke"
    require(example_root.is_dir(), "public_consumer_example_missing")
    files = [
        example_root / "CMakeLists.txt",
        example_root / "main.c",
    ]
    for path in files:
        scan_text_file(path, project_root)
    cmake_text = files[0].read_text(encoding="utf-8")
    main_text = files[1].read_text(encoding="utf-8")
    require("PUBLIC_CONSUMER_SMOKE" in cmake_text, "public_consumer_marker_missing")
    require("find_package(ScratchBirdEngine CONFIG REQUIRED)" in cmake_text,
            "public_consumer_find_package_missing")
    require("#include \"scratchbird/engine/engine.h\"" in main_text,
            "public_consumer_header_missing")
    require("sb_engine_open" in main_text and "sb_engine_dispatch_sblr" in main_text,
            "public_consumer_runtime_calls_missing")
    return [rel(path, project_root) for path in files]


def validate_install(prefix: Path) -> dict[str, Any]:
    installed = [installed_engine_library(prefix)]
    for required in REQUIRED_INSTALL_FILES:
        installed.append(require_file(prefix / required, prefix, required))
    for forbidden in FORBIDDEN_ENGINE_ONLY_ARTIFACTS:
        require(not (prefix / forbidden).exists(),
                f"unexpected_non_engine_artifact:{forbidden}")
    for cmake_file in (
        prefix / "lib" / "cmake" / "ScratchBirdEngine" / "ScratchBirdEngineConfig.cmake",
        prefix / "lib" / "cmake" / "ScratchBirdEngine" / "ScratchBirdEngineTargets.cmake",
    ):
        scan_text_file(cmake_file, prefix)
    targets_text = (
        prefix / "lib" / "cmake" / "ScratchBirdEngine" / "ScratchBirdEngineTargets.cmake"
    ).read_text(encoding="utf-8")
    require("ScratchBird::sb_engine" in targets_text,
            "installed_cmake_target_missing")
    for forbidden in (
        "sb_server",
        "sb_listener",
        "sbmn_manager",
        "sbp_sbsql",
        "sbu_sbsql_parser_support",
    ):
        require(forbidden not in targets_text,
                f"installed_cmake_private_target_leak:{forbidden}")
    return {
        "installed_required_files": sorted(installed),
        "forbidden_non_engine_artifacts_absent": sorted(FORBIDDEN_ENGINE_ONLY_ARTIFACTS),
    }


def run_external_consumer(args: argparse.Namespace, prefix: Path, work_root: Path) -> dict[str, str]:
    example_source = args.project_root / "examples" / "public_engine_consumer_smoke"
    consumer_build = work_root / "consumer-build"
    run(
        [
            str(args.cmake),
            "-S",
            str(example_source),
            "-B",
            str(consumer_build),
            f"-DCMAKE_PREFIX_PATH={prefix}",
        ],
        cwd=args.project_root,
    )
    run([str(args.cmake), "--build", str(consumer_build)], cwd=args.project_root)
    executable_name = "scratchbird_public_engine_consumer_smoke"
    if sys.platform == "win32":
        executable_name += ".exe"
    candidates = [
        consumer_build / executable_name,
        consumer_build / "Release" / executable_name,
        consumer_build / "RelWithDebInfo" / executable_name,
        consumer_build / "Debug" / executable_name,
    ]
    executable = next((path for path in candidates if path.exists() and path.is_file()), None)
    if executable is None:
        executable = next(
            (path for path in consumer_build.rglob(executable_name)
             if path.exists() and path.is_file()),
            None,
        )
    require(executable is not None and executable.exists() and executable.is_file(),
            "consumer_executable_missing")
    env = os.environ.copy()
    if sys.platform == "win32":
        env["PATH"] = str(prefix / "bin") + os.pathsep + env.get("PATH", "")
    else:
        env["LD_LIBRARY_PATH"] = (
            str(prefix / "lib") + os.pathsep + env.get("LD_LIBRARY_PATH", "")
        )
    run([str(executable)], cwd=args.project_root, env=env)
    return {
        "consumer_build": rel(consumer_build, work_root),
        "consumer_executable": rel(executable, work_root),
        "runtime_library_path": "staged_prefix",
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
    prefix = work_root / "install"

    run([str(args.cmake), "--build", str(build_root), "--target", "sb_engine_shared"],
        cwd=project_root)
    run([str(args.cmake), "--install", str(build_root), "--prefix", str(prefix)],
        cwd=project_root)

    example_files = validate_example_source(project_root)
    install_evidence = validate_install(prefix)
    consumer_evidence = run_external_consumer(args, prefix, work_root)

    evidence: dict[str, Any] = {
        "schema_version": 1,
        "gate": "PCR-GATE-120",
        "policy": {
            "public_tree_inputs_only": True,
            "private_docs_required": False,
            "git_history_required": False,
            "first_release_binary_scope": "engine_only",
            "cluster_production_execution": "external_provider_only_not_used",
        },
        "example": {
            "marker": "PUBLIC_CONSUMER_SMOKE",
            "files": example_files,
        },
        "install": install_evidence,
        "consumer": consumer_evidence,
    }
    return evidence


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
    print(f"public_install_consumer_output={output.relative_to(args.build_root.resolve()).as_posix()}")
    print("public_install_consumer_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
