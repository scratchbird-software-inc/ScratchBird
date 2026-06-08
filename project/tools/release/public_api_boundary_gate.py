#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate public API, include, target, and tool boundaries."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
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

HEADER_SUFFIXES = {".h", ".hh", ".hpp", ".hxx"}
PUBLIC_HEADER_ROOT = Path("project") / "include" / "scratchbird" / "engine"


def fail(message: str) -> None:
    print(f"public_api_boundary_gate=fail:{message}", file=sys.stderr)
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


def check_cmake_boundary(repo_root: Path, project_root: Path) -> list[dict[str, Any]]:
    cmake_text = require_file(project_root / "CMakeLists.txt", repo_root)
    for token in (
        "PUBLIC_TARGET_EXPORTS",
        "SB_INSTALL_NON_ENGINE_COMPONENTS",
        "Install non-engine listener parser manager driver UDR example and auxiliary artifacts",
        "install(TARGETS sb_engine_shared",
        "EXPORT ScratchBirdEngineTargets",
        "install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/scratchbird/engine",
        "install(EXPORT ScratchBirdEngineTargets",
        "NAMESPACE ScratchBird::",
        "ScratchBirdEngineConfig.cmake",
        "if(SB_INSTALL_NON_ENGINE_COMPONENTS)",
        "sb_server",
        "sb_listener",
        "sbmn_manager",
        "sbp_sbsql",
        "sbu_sbsql_parser_support",
        "COMPONENT non_engine_runtime",
        "DriverPackageManifest.csv",
    ):
        require_contains(cmake_text, token, "project_cmake")

    if re.search(r"option\(SB_INSTALL_NON_ENGINE_COMPONENTS[^\n]*\n[^\n]*ON\)", cmake_text):
        fail("non_engine_install_default_must_be_off")
    if "install(TARGETS sb_engine_internal_api" in cmake_text:
        fail("internal_api_target_install_leak")
    if "install(TARGETS sb_cluster_provider" in cmake_text:
        fail("cluster_provider_target_install_leak")

    config_text = require_file(project_root / "cmake" / "ScratchBirdEngineConfig.cmake.in", repo_root)
    for token in (
        "ScratchBirdEngineTargets.cmake",
        "ScratchBird::sb_engine",
    ):
        require_contains(config_text, token, "engine_config")
    for banned in (
        "sb_engine_internal_api",
        "sb_cluster_provider",
        "sb_server",
        "sb_listener",
        "sbmn_manager",
        "sbp_sbsql",
    ):
        if banned in config_text:
            fail(f"engine_config_private_target_leak:{banned}")

    sbsql_worker_cmake = require_file(
        project_root / "src" / "parsers" / "sbsql_worker" / "CMakeLists.txt",
        repo_root,
    )
    for token in (
        "if(SB_INSTALL_NON_ENGINE_COMPONENTS)",
        "install(TARGETS sbp_sbsql",
    ):
        require_contains(sbsql_worker_cmake, token, "sbsql_worker_cmake")
    if "install(TARGETS sbp_sbsql\n  RUNTIME DESTINATION bin\n)" in sbsql_worker_cmake:
        fail("sbsql_worker_install_unguarded")

    udr_support_cmake = require_file(
        project_root / "src" / "udr" / "sbu_sbsql_parser_support" / "CMakeLists.txt",
        repo_root,
    )
    for token in (
        "if(SB_INSTALL_NON_ENGINE_COMPONENTS)",
        "install(TARGETS sbu_sbsql_parser_support",
    ):
        require_contains(udr_support_cmake, token, "udr_support_cmake")
    if "install(TARGETS sbu_sbsql_parser_support\n  ARCHIVE DESTINATION lib" in udr_support_cmake:
        fail("udr_support_install_unguarded")

    return [
        {"surface": "cmake_install", "status": "engine_only_by_default"},
        {"surface": "cmake_package", "status": "public_engine_target_only"},
        {"surface": "sbsql_worker_install", "status": "non_engine_guarded"},
        {"surface": "sbsql_udr_support_install", "status": "non_engine_guarded"},
    ]


def iter_public_headers(project_root: Path):
    root = project_root / "include" / "scratchbird" / "engine"
    if not root.is_dir():
        fail("public_engine_header_root_missing")
    for path in sorted(root.rglob("*")):
        if path.is_file() and path.suffix in HEADER_SUFFIXES:
            yield path


def check_header_includes(path: Path, text: str, repo_root: Path) -> None:
    include_re = re.compile(r"^\s*#\s*include\s+[<\"]([^>\"]+)[>\"]", re.MULTILINE)
    for match in include_re.finditer(text):
        include = match.group(1)
        reject_private_reference(include, f"include:{rel(path, repo_root)}")
        if include.startswith("../") or "/../" in include:
            fail(f"relative_parent_include_leak:{rel(path, repo_root)}:{include}")
        banned_fragments = (
            "project/src",
            "src/",
            "engine/internal_api",
            "cluster_provider",
            "manager/",
            "listener/",
            "parsers/",
            "udr/",
            "tests/",
        )
        for fragment in banned_fragments:
            if fragment in include:
                fail(f"internal_include_leak:{rel(path, repo_root)}:{include}")


def check_public_headers(repo_root: Path, project_root: Path) -> dict[str, Any]:
    files = list(iter_public_headers(project_root))
    if not files:
        fail("public_engine_headers_empty")
    banned_tokens = (
        "test_mode",
        "sb_engine_internal_api",
        "engine/internal_api",
        "cluster_provider",
        "sb_cluster_provider",
        "ScratchBird" "Private",
        "Trade secret/private",
        "No rights are granted",
    )
    digest = hashlib.sha256()
    for path in files:
        text = require_file(path, repo_root)
        path_text = rel(path, repo_root)
        reject_private_reference(path_text, "public_header_path")
        digest.update(path_text.encode("utf-8"))
        digest.update(b"\0")
        digest.update(hashlib.sha256(text.encode("utf-8")).hexdigest().encode("ascii"))
        digest.update(b"\0")
        for fragment in FORBIDDEN_REFERENCE_FRAGMENTS:
            if fragment in text:
                fail(f"private_reference_in_public_header:{path_text}")
        for token in banned_tokens:
            if token in text:
                fail(f"public_header_internal_token:{path_text}:{token}")
        check_header_includes(path, text, repo_root)
    return {
        "header_root": PUBLIC_HEADER_ROOT.as_posix(),
        "header_count": len(files),
        "aggregate_sha256": digest.hexdigest(),
        "status": "passed",
    }


def check_public_tools(repo_root: Path, project_root: Path) -> dict[str, Any]:
    tools_root = project_root / "tools" / "release"
    if not tools_root.is_dir():
        fail("public_release_tools_root_missing")
    files = sorted(path for path in tools_root.iterdir() if path.is_file())
    banned_tokens = (
        "docs" + "/" + "execution-plans",
        "docs" + "/" + "completed-execution-plans",
        "docs" + "/" + "findings",
        "git" + " log",
        "git" + " show",
        "git" + " rev-parse",
    )
    for path in files:
        path_text = rel(path, repo_root)
        reject_private_reference(path_text, "public_tool_path")
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        for token in banned_tokens:
            if token in text:
                fail(f"public_tool_private_input_leak:{path_text}:{token}")
    return {"tool_count": len(files), "status": "passed"}


def build_evidence(args: argparse.Namespace) -> dict[str, Any]:
    repo_root = args.repo_root.resolve()
    project_root = args.project_root.resolve()
    build_root = args.build_root.resolve()
    output = args.output.resolve()
    if not repo_root.is_dir() or not project_root.is_dir() or not build_root.is_dir():
        fail("input_root_missing")
    try:
        output_record = output.relative_to(build_root).as_posix()
    except ValueError:
        fail("output_must_be_under_build_root")
    reject_private_reference(output_record, "output")

    evidence: dict[str, Any] = {
        "schema_version": 1,
        "policy": {
            "public_tree_inputs_only": True,
            "private_docs_required": False,
            "git_history_required": False,
            "first_release_binary_scope": "engine_only",
            "public_cmake_target": "ScratchBird::sb_engine",
            "cluster_provider_boundary": "not_exported_as_public_engine_target",
            "release_proof_is_evidence_only": True,
        },
        "cmake_boundary": check_cmake_boundary(repo_root, project_root),
        "public_headers": check_public_headers(repo_root, project_root),
        "public_tools": check_public_tools(repo_root, project_root),
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
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    evidence = build_evidence(args)
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"public_api_boundary_output={output.relative_to(args.build_root.resolve()).as_posix()}")
    print(f"public_api_boundary_sha256={evidence['evidence_sha256']}")
    print("public_api_boundary_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
