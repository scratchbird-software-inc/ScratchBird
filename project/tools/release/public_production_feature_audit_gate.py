#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate public production-build feature refusal gates."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
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

REQUIRED_CONTROL_TOKENS = (
    "SB_AGENT_ALLOW_FIXTURE_AUTH_IN_PRODUCTION",
    "SB_AGENT_ALLOW_FIXTURE_POLICY_IN_PRODUCTION",
    "SB_AGENT_ALLOW_TEST_SEEDS_IN_PRODUCTION",
    "SB_AGENT_ALLOW_FORCED_COLLISION_HOOKS_IN_PRODUCTION",
    "SB_AGENT_ALLOW_DEBUG_ONLY_PATHS_IN_PRODUCTION",
    "SB_AGENT_CLUSTER_STUB_LIVE_CLAIMS",
    "SB_OPTIMIZER_ALLOW_FIXTURE_STATS_IN_PRODUCTION",
    "SB_OPTIMIZER_ALLOW_LOCAL_DEFAULT_STATS_IN_PRODUCTION",
    "SB_OPTIMIZER_ALLOW_POLICY_DEFAULT_STATS_IN_PRODUCTION",
    "SB_OPTIMIZER_ALLOW_PLACEHOLDER_RUNTIME_EVIDENCE_IN_PRODUCTION",
    "SB_OPTIMIZER_ALLOW_DONOR_AUTHORITY_IN_PRODUCTION",
    "SB_OPTIMIZER_ALLOW_PARSER_SHORTCUTS_IN_PRODUCTION",
    "SB_OPTIMIZER_ALLOW_CLUSTER_STUB_LIVE_CLAIMS",
    "SB_OPTIMIZER_ALLOW_DEBUG_ONLY_PATHS_IN_PRODUCTION",
    "SB_COMMERCIAL_ALLOW_FIXTURE_AUTH_IN_PRODUCTION",
    "SB_COMMERCIAL_ALLOW_TEST_SEEDS_IN_PRODUCTION",
    "SB_COMMERCIAL_ALLOW_FORCED_COLLISION_HOOKS_IN_PRODUCTION",
    "SB_COMMERCIAL_ALLOW_STUB_PROVIDERS_IN_PRODUCTION",
    "SB_COMMERCIAL_ALLOW_DEBUG_CREDENTIALS_IN_PRODUCTION",
    "SB_COMMERCIAL_ALLOW_DEBUG_ONLY_AUTHORITY_PATHS_IN_PRODUCTION",
    "SB_COMMERCIAL_ALLOW_NO_CLUSTER_PRODUCTION_CLAIMS",
    "SB_COMMERCIAL_CLUSTER_PRODUCTION_CLAIMS",
    "SB_CLUSTER_PROVIDER_STUB",
    "SCRATCHBIRD_ENABLE_DEBUG_LOGS",
    "SCRATCHBIRD_ENABLE_HOTPATH_TRACE",
    "SCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE",
    "SCRATCHBIRD_ENABLE_PREPARED_TRACE",
)

GATE_SCRIPTS = (
    "AgentProductionBuildGate.cmake",
    "AgentProductionBuildGateMatrix.cmake",
    "AgentProductionConfigureGateMatrix.cmake",
    "OptimizerProductionBuildGate.cmake",
    "OptimizerProductionBuildGateMatrix.cmake",
    "OptimizerProductionConfigureGateMatrix.cmake",
    "CommercialReadinessProductionBuildGate.cmake",
    "CommercialReadinessProductionBuildGateMatrix.cmake",
)


def fail(message: str) -> None:
    print(f"public_production_feature_audit_gate=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def rel(path: Path, root: Path) -> str:
    return path.relative_to(root).as_posix()


def reject_private_reference(value: str, context: str) -> None:
    if Path(value).is_absolute():
        fail(f"absolute_path_recorded:{context}:{value}")
    for fragment in FORBIDDEN_REFERENCE_FRAGMENTS:
        if fragment in value:
            fail(f"private_reference_recorded:{context}:{value}")


def require_file(path: Path, repo_root: Path) -> str:
    if not path.exists() or not path.is_file():
        fail(f"required_file_missing:{rel(path, repo_root)}")
    return path.read_text(encoding="utf-8")


def require_contains(text: str, token: str, context: str) -> None:
    if token not in text:
        fail(f"{context}_missing:{token}")


def run_command(
    command: list[str],
    cwd: Path,
    label: str,
    env: dict[str, str],
) -> dict[str, Any]:
    proc = subprocess.run(
        command,
        cwd=cwd,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    output = proc.stdout or ""
    if proc.returncode != 0:
        print(output, file=sys.stderr)
        fail(f"command_failed:{label}:exit={proc.returncode}")
    return {
        "label": label,
        "tool": Path(command[0]).name,
        "returncode": proc.returncode,
        "stdout_sha256": sha256_text(output.replace("\r\n", "\n")),
        "status": "passed",
    }


def check_project_controls(repo_root: Path, project_root: Path) -> list[dict[str, Any]]:
    cmake_text = require_file(project_root / "CMakeLists.txt", repo_root)
    for token in REQUIRED_CONTROL_TOKENS:
        require_contains(cmake_text, token, "project_cmake")
    for token in (
        "PUBLIC_PRODUCTION_FEATURE_AUDIT",
        "SB_AGENT_ENFORCE_PRODUCTION_BUILD_GATE cannot be disabled for release-complete builds",
        "SB_OPTIMIZER_ENFORCE_PRODUCTION_BUILD_GATE cannot be disabled for release-complete builds",
        "SB_COMMERCIAL_ENFORCE_PRODUCTION_BUILD_GATE cannot be disabled for release-complete builds",
        "SCRATCHBIRD_AGENT_PRODUCTION_BUILD",
        "SCRATCHBIRD_OPTIMIZER_PRODUCTION_BUILD",
        "SCRATCHBIRD_COMMERCIAL_READINESS_PRODUCTION_BUILD",
    ):
        require_contains(cmake_text, token, "project_cmake")

    release_cmake = require_file(project_root / "tests" / "release" / "CMakeLists.txt", repo_root)
    for token in (
        "PUBLIC_PRODUCTION_BUILD_GATE",
        "public_production_feature_audit_gate",
        "public_production_feature_audit_gate.py",
        "PCR-GATE-113",
        "PCR-113",
    ):
        require_contains(release_cmake, token, "release_ctest")

    return [
        {"surface": "project_cmake", "status": "controls_declared"},
        {"surface": "release_ctest", "status": "public_gate_registered"},
    ]


def check_gate_script_coverage(repo_root: Path, project_root: Path) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    cmake_root = project_root / "cmake"
    for script in GATE_SCRIPTS:
        path = cmake_root / script
        text = require_file(path, repo_root)
        for fragment in FORBIDDEN_REFERENCE_FRAGMENTS:
            if fragment in text:
                fail(f"private_reference_in_gate_script:{script}")
        path_text = rel(path, repo_root)
        reject_private_reference(path_text, "gate_script_path")
        records.append(
            {
                "script": path_text,
                "sha256": hashlib.sha256(text.encode("utf-8")).hexdigest(),
                "status": "present",
            }
        )

    combined = "\n".join(require_file(cmake_root / script, repo_root) for script in GATE_SCRIPTS)
    required_cases = (
        "fixture_auth",
        "test_seeds",
        "forced_collision_hooks",
        "debug_logs",
        "cluster_stub_link",
        "placeholder_evidence",
        "local_defaults",
        "stub_providers",
        "debug_credentials",
        "no_cluster_claim",
        "parser_shortcut",
        "donor_authority",
        "synthetic_feedback",
    )
    for token in REQUIRED_CONTROL_TOKENS:
        require_contains(combined, token, "gate_script_matrix")
    for case in required_cases:
        require_contains(combined, case, "gate_script_matrix")
    return records


def llvm_cmake_definitions_from_env() -> list[str]:
    definitions: list[str] = []
    for name in (
        "SB_LLVM_PROJECT_ROOT",
        "SB_LLVM_TOOLS_ROOT",
        "SB_LLVM_LIBRARY",
        "SB_LLVM_LINK_MODE",
    ):
        value = os.environ.get(name, "")
        if value:
            definitions.append(f"-D{name}={value}")
    return definitions


def matrix_commands(args: argparse.Namespace, project_root: Path) -> list[tuple[str, list[str]]]:
    cmake_root = project_root / "cmake"
    llvm_definitions = llvm_cmake_definitions_from_env()
    return [
        (
            "agent_build_matrix",
            [args.cmake, "-P", str(cmake_root / "AgentProductionBuildGateMatrix.cmake")],
        ),
        (
            "agent_configure_matrix",
            [
                args.cmake,
                f"-DSB_PROJECT_SOURCE_DIR={project_root}",
                f"-DSB_CONFIGURE_GATE_BINARY_ROOT={args.work_root / 'agent_configure'}",
                *llvm_definitions,
                "-P",
                str(cmake_root / "AgentProductionConfigureGateMatrix.cmake"),
            ],
        ),
        (
            "optimizer_build_matrix",
            [args.cmake, "-P", str(cmake_root / "OptimizerProductionBuildGateMatrix.cmake")],
        ),
        (
            "optimizer_configure_matrix",
            [
                args.cmake,
                f"-DSB_PROJECT_SOURCE_DIR={project_root}",
                f"-DSB_CONFIGURE_GATE_BINARY_ROOT={args.work_root / 'optimizer_configure'}",
                *llvm_definitions,
                "-P",
                str(cmake_root / "OptimizerProductionConfigureGateMatrix.cmake"),
            ],
        ),
        (
            "commercial_build_matrix",
            [args.cmake, "-P", str(cmake_root / "CommercialReadinessProductionBuildGateMatrix.cmake")],
        ),
    ]


def run_matrices(args: argparse.Namespace, repo_root: Path, project_root: Path) -> list[dict[str, Any]]:
    env = os.environ.copy()
    if args.c_compiler:
        env["CC"] = args.c_compiler
    if args.cxx_compiler:
        env["CXX"] = args.cxx_compiler
    args.work_root.mkdir(parents=True, exist_ok=True)
    records: list[dict[str, Any]] = []
    for label, command in matrix_commands(args, project_root):
        records.append(run_command(command, repo_root, label, env))
    return records


def build_evidence(args: argparse.Namespace) -> dict[str, Any]:
    repo_root = args.repo_root.resolve()
    project_root = args.project_root.resolve()
    build_root = args.build_root.resolve()
    args.work_root = args.work_root.resolve()
    output = args.output.resolve()
    if not repo_root.is_dir() or not project_root.is_dir() or not build_root.is_dir():
        fail("input_root_missing")
    if project_root.name != "project":
        fail("project_root_must_be_project_directory")
    try:
        output_record = output.relative_to(build_root).as_posix()
    except ValueError:
        fail("output_must_be_under_build_root")
    reject_private_reference(output_record, "output")

    controls = check_project_controls(repo_root, project_root)
    script_coverage = check_gate_script_coverage(repo_root, project_root)
    matrix_results = run_matrices(args, repo_root, project_root)

    evidence: dict[str, Any] = {
        "schema_version": 1,
        "policy": {
            "public_tree_inputs_only": True,
            "private_docs_required": False,
            "git_history_required": False,
            "production_profile": "release-complete",
            "cluster_stub_execution": "forbidden_in_production",
            "no_cluster_production_claims": "forbidden",
            "release_proof_is_evidence_only": True,
        },
        "feature_classes": [
            "fixture_auth",
            "test_seeds",
            "forced_collision_hooks",
            "debug_traces",
            "stub_cluster_execution",
            "placeholder_optimizer_evidence",
            "private_proof_hooks",
            "local_default_statistics",
        ],
        "controls": controls,
        "gate_scripts": script_coverage,
        "matrix_results": matrix_results,
    }
    evidence["evidence_sha256"] = sha256_text(
        json.dumps(evidence, sort_keys=True, separators=(",", ":"))
    )
    return evidence


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--work-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--c-compiler", default="")
    parser.add_argument("--cxx-compiler", default="")
    args = parser.parse_args()

    evidence = build_evidence(args)
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"public_production_feature_audit_output={output.relative_to(args.build_root.resolve()).as_posix()}")
    print(f"public_production_feature_audit_sha256={evidence['evidence_sha256']}")
    print("public_production_feature_audit_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
