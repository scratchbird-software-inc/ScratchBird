#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

"""Fail closed if the production SBParser target regains engine dependencies."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import tempfile


FORBIDDEN_TARGET_FAMILIES = (
    "sb_server",
    "sb_engine",
    "sb_storage",
    "sb_transaction",
    "sb_core_agents",
    "sb_core_catalog",
    "sb_core_index",
)


def fail(message: str) -> None:
    raise AssertionError(message)


def function_block(text: str, function_name: str) -> str:
    match = re.search(
        rf"(?ms)^function\s*\(\s*{re.escape(function_name)}\b.*?"
        rf"^endfunction\s*\(\s*\)",
        text,
    )
    if match is None:
        fail(f"cmake_function_missing:{function_name}")
    return match.group(0)


def set_block(text: str, variable: str) -> str:
    match = re.search(
        rf"(?ms)^set\s*\(\s*{re.escape(variable)}\b(.*?)^\s*\)",
        text,
    )
    if match is None:
        fail(f"cmake_set_missing:{variable}")
    return match.group(0)


def target_block(text: str, target: str, next_anchor: str) -> str:
    start_token = f"add_library({target}"
    start = text.find(start_token)
    if start < 0:
        fail(f"cmake_target_missing:{target}")
    end = text.find(next_anchor, start)
    if end < 0:
        fail(f"cmake_target_end_missing:{target}")
    return text[start:end]


def require_source_partition(cmake_text: str) -> str:
    production_sources = set_block(
        cmake_text, "SBSQL_PARSER_WORKER_CORE_SOURCES"
    )
    if "lifecycle/agent_parser_interface_bridge.cpp" in production_sources:
        fail("engine_lifecycle_bridge_in_production_parser_sources")

    configure_function = function_block(
        cmake_text, "sbsql_configure_parser_worker_core"
    )
    for forbidden in FORBIDDEN_TARGET_FAMILIES:
        if forbidden in configure_function:
            fail(f"forbidden_production_parser_link:{forbidden}")

    for guarded_target in (
        "sbl_sbsql_parser_worker_core",
        "sbp_sbsql",
    ):
        invocation = (
            "sbsql_assert_standalone_parser_link_closure("
            f"{guarded_target})"
        )
        if invocation not in cmake_text:
            fail(f"production_parser_guard_invocation_missing:{guarded_target}")

    supervisor_bridge = target_block(
        cmake_text,
        "sb_engine_parser_lifecycle_bridge",
        "# Embedded client mode",
    )
    for required in (
        "lifecycle/agent_parser_interface_bridge.cpp",
        "sbl_sbsql_parser_worker_core",
        "sb_core_agents",
    ):
        if required not in supervisor_bridge:
            fail(f"engine_lifecycle_bridge_partition_missing:{required}")

    guard = function_block(
        cmake_text, "sbsql_assert_standalone_parser_link_closure"
    )
    for forbidden in FORBIDDEN_TARGET_FAMILIES:
        if forbidden not in guard:
            fail(f"guard_forbidden_family_missing:{forbidden}")
    for wrapper in ("LINK_ONLY", "BUILD_INTERFACE"):
        if wrapper not in guard:
            fail(f"guard_generator_expression_missing:{wrapper}")
    return guard


def configure(
    cmake: Path,
    source: Path,
    build: Path,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(cmake), "-S", str(source), "-B", str(build)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )


def synthetic_project(
    guard: str,
    *,
    forbidden_target: str | None = None,
    wrapper: str | None = None,
) -> str:
    lines = [
        "cmake_minimum_required(VERSION 3.25)",
        "project(sbparser_graph_contract LANGUAGES NONE)",
        guard,
        "add_library(sbl_parser_server_ipc_schema INTERFACE)",
        "add_library(sbl_listener_control_plane INTERFACE)",
        "add_library(sb_core_datatypes INTERFACE)",
        "add_library(sb_core_uuid INTERFACE)",
        "add_library(allowed_mid INTERFACE)",
        (
            "target_link_libraries(allowed_mid INTERFACE "
            + "sbl_parser_server_ipc_schema sbl_listener_control_plane "
            + "sb_core_datatypes sb_core_uuid)"
        ),
        "add_library(sbl_sbsql_parser_pipeline INTERFACE)",
        (
            "target_link_libraries(sbl_sbsql_parser_pipeline INTERFACE "
            + "sbl_parser_server_ipc_schema)"
        ),
        "add_library(sbl_sbsql_parser_worker_core INTERFACE)",
        (
            "target_link_libraries(sbl_sbsql_parser_worker_core INTERFACE "
            + "sbl_sbsql_parser_pipeline allowed_mid)"
        ),
        "add_library(sbp_sbsql INTERFACE)",
    ]
    if forbidden_target is None:
        lines.extend(
            (
                (
                    "target_link_libraries(sbp_sbsql INTERFACE "
                    + "sbl_sbsql_parser_worker_core)"
                ),
                "sbsql_assert_standalone_parser_link_closure(sbp_sbsql)",
            )
        )
        return "\n".join(lines) + "\n"

    lines.append(f"add_library({forbidden_target} INTERFACE)")
    if wrapper is None:
        lines.append(
            f"target_link_libraries(allowed_mid INTERFACE {forbidden_target})"
        )
    else:
        lines.append(
            "set_property(TARGET allowed_mid PROPERTY "
            f'INTERFACE_LINK_LIBRARIES "$<{wrapper}:{forbidden_target}>")'
        )
    lines.extend(
        (
            (
                "target_link_libraries(sbp_sbsql INTERFACE "
                + "sbl_sbsql_parser_worker_core)"
            ),
            "sbsql_assert_standalone_parser_link_closure(sbp_sbsql)",
        )
    )
    return "\n".join(lines) + "\n"


def run_guard_mutation_matrix(cmake: Path, guard: str) -> None:
    with tempfile.TemporaryDirectory(prefix="sbparser-graph-contract-") as temp:
        root = Path(temp)
        allowed_source = root / "allowed-source"
        allowed_source.mkdir()
        (allowed_source / "CMakeLists.txt").write_text(
            synthetic_project(guard), encoding="utf-8"
        )
        allowed = configure(cmake, allowed_source, root / "allowed-build")
        if allowed.returncode != 0:
            fail(f"allowed_graph_rejected:{allowed.stdout[-4000:]}")

        engine_diagnostic = (
            "SBParser standalone worker link closure reaches forbidden engine"
        )
        cases = [
            (f"{family}_forbidden_fixture", None, engine_diagnostic)
            for family in FORBIDDEN_TARGET_FAMILIES
        ]
        cases.extend(
            (
                (
                    "sb_storage_link_only_fixture",
                    "LINK_ONLY",
                    engine_diagnostic,
                ),
                (
                    "sb_transaction_build_interface_fixture",
                    "BUILD_INTERFACE",
                    engine_diagnostic,
                ),
                (
                    "sbl_firebird_parser_pipeline",
                    None,
                    "SBParser standalone worker link closure reaches forbidden "
                    "cross-parser target",
                ),
            )
        )
        for index, (target, wrapper, diagnostic) in enumerate(cases):
            source = root / f"refused-source-{index}"
            source.mkdir()
            (source / "CMakeLists.txt").write_text(
                synthetic_project(
                    guard,
                    forbidden_target=target,
                    wrapper=wrapper,
                ),
                encoding="utf-8",
            )
            refused = configure(cmake, source, root / f"refused-build-{index}")
            if refused.returncode == 0:
                fail(f"forbidden_graph_accepted:{target}:{wrapper or 'transitive'}")
            normalized_output = " ".join(refused.stdout.split())
            if diagnostic not in normalized_output:
                fail(
                    f"forbidden_graph_wrong_diagnostic:{target}:"
                    f"{refused.stdout[-4000:]}"
                )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--cmake", type=Path, required=True)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    cmake = args.cmake.resolve()
    cmake_file = (
        repo_root / "project/src/parsers/sbsql_worker/CMakeLists.txt"
    )
    cmake_text = cmake_file.read_text(encoding="utf-8")
    guard = require_source_partition(cmake_text)
    run_guard_mutation_matrix(cmake, guard)
    print("standalone_sbparser_graph_contract_test=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
