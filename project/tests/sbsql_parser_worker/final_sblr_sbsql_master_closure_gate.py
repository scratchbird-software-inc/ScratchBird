#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Master consistency gate for the current SBsql/SBLR closure sequence.

The master gate composes the language and enterprise checks, verifies their
CTest dependency DAG, and cross-checks the derived population equations.  A
pass means that authority and obligation accounting are coherent; the active
workplan remains explicitly in progress until its independent acceptance
phases close.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

import final_sblr_sbsql_enterprise_proof_closure_gate as enterprise_gate


LANGUAGE_TEST = "sbsql_final_language_expansion_closure_gate"
ENTERPRISE_TEST = "final_sblr_sbsql_enterprise_proof_closure_gate"
MASTER_TEST = "final_sblr_sbsql_master_closure_gate"

ENTERPRISE_BASE_DEPENDENCIES = {
    LANGUAGE_TEST,
    "ctest_no_execution_plan_runtime_dependency_gate",
    "sblr_surface_fse_p7_execution_proof_gate",
}
MASTER_BASE_DEPENDENCIES = {ENTERPRISE_TEST, LANGUAGE_TEST}
COMPATIBILITY_DEPENDENCY = "parser_dialect_isolation_audit_gate"

REQUIRED_SCRIPTS = {
    LANGUAGE_TEST: "project/tests/sbsql_parser_worker/sbsql_final_language_expansion_closure_gate.py",
    ENTERPRISE_TEST: (
        "project/tests/sbsql_parser_worker/final_sblr_sbsql_enterprise_proof_closure_gate.py"
    ),
    MASTER_TEST: "project/tests/sbsql_parser_worker/final_sblr_sbsql_master_closure_gate.py",
}


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def generated_dependencies(build_root: Path, test_name: str) -> set[str]:
    for ctest_file in build_root.rglob("CTestTestfile.cmake"):
        text = ctest_file.read_text(encoding="utf-8", errors="replace")
        match = re.search(
            rf"set_tests_properties\(\[=*\[{re.escape(test_name)}\]=*\]"
            rf"[^\n]*\bDEPENDS \"([^\"]*)\"",
            text,
        )
        if match:
            return {item for item in match.group(1).split(";") if item}
    fail(f"generated CTest dependency property missing for {test_name}")


def validate_dependency_dag(root: Path, build_root: Path | None) -> str:
    cmake = root / "project/tests/sbsql_parser_worker/CMakeLists.txt"
    require(cmake.is_file(), f"SBsql parser-worker CMake registration missing: {cmake}")
    source = cmake.read_text(encoding="utf-8", errors="replace")
    for test_name, relative in REQUIRED_SCRIPTS.items():
        require((root / relative).is_file(), f"closure gate script missing: {relative}")
        require(test_name in source, f"closure gate CTest registration missing: {test_name}")
    for token in (
        "SBSQL_FINAL_ENTERPRISE_PROOF_DEPENDS",
        "SBSQL_FINAL_MASTER_CLOSURE_DEPENDS",
        "if(SB_BUILD_COMPATIBILITY_PARSERS)",
        COMPATIBILITY_DEPENDENCY,
    ):
        require(token in source, f"closure dependency source registration missing {token}")

    if build_root is None:
        return "source_only"

    compatibility_enabled = enterprise_gate.cmake_cache_bool(
        build_root, "SB_BUILD_COMPATIBILITY_PARSERS"
    )
    expected_enterprise = set(ENTERPRISE_BASE_DEPENDENCIES)
    expected_master = set(MASTER_BASE_DEPENDENCIES)
    if compatibility_enabled:
        expected_enterprise.add(COMPATIBILITY_DEPENDENCY)
        expected_master.add(COMPATIBILITY_DEPENDENCY)

    observed_enterprise = generated_dependencies(build_root, ENTERPRISE_TEST)
    observed_master = generated_dependencies(build_root, MASTER_TEST)
    require(
        observed_enterprise == expected_enterprise,
        "generated enterprise closure dependency set drift: "
        f"expected={sorted(expected_enterprise)} observed={sorted(observed_enterprise)}",
    )
    require(
        observed_master == expected_master,
        "generated master closure dependency set drift: "
        f"expected={sorted(expected_master)} observed={sorted(observed_master)}",
    )
    return "compatibility_enabled" if compatibility_enabled else "sbsql_only"


def validate_master(root: Path, build_root: Path | None) -> dict[str, int | str]:
    summary = enterprise_gate.validate_enterprise(root, build_root)
    dependency_profile = validate_dependency_dag(root, build_root)
    require(
        summary["implementation_items"]
        == summary["command_identities"]
        + summary["opcode_identities"]
        + summary["envelope_identities"],
        "implementation population does not equal Core command + opcode + envelope identities",
    )
    require(
        summary["test_obligations"]
        == 4 * (summary["command_identities"] + summary["opcode_identities"]),
        "test population does not provide four layers per Core command/opcode identity",
    )
    require(
        summary["workplan_status"] == "in_progress",
        "master gate must not turn structural closure into final workplan acceptance",
    )
    summary["dependency_profile"] = dependency_profile
    return summary


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--build-root")
    args = parser.parse_args()
    root = Path(args.repo_root).resolve()
    build_root = Path(args.build_root).resolve() if args.build_root else None

    summary = validate_master(root, build_root)
    print(
        "final_sblr_sbsql_master_closure_gate=passed "
        f"dependency_profile={summary['dependency_profile']} "
        f"surfaces={summary['surface_rows']} "
        f"command_identities={summary['command_identities']} "
        f"sblr_opcodes={summary['opcode_identities']} "
        f"sblr_envelopes={summary['envelope_identities']} "
        f"implementation_items={summary['implementation_items']} "
        f"test_obligations={summary['test_obligations']} "
        f"open_release_blockers={summary['open_release_blockers']} "
        f"workplan_status={summary['workplan_status']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
