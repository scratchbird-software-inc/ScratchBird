#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate the public reference-system material boundary.

The public repository may track ScratchBird-owned compatibility implementation,
CTest harnesses, locators, acquisition manifests, and reference-regression
metadata. It must not track raw upstream regression payloads, downloaded/native
tools, upstream source trees, or stale pre-rename public terminology.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys


# PUBLIC_REFERENCE_MATERIAL_BOUNDARY_GATE

REQUIRED_IGNORE_PATTERNS = (
    "project/src/**/[d]onor*/",
    "project/src/**/*[d]onor*",
    "project/src/**/*[D]onor*",
    "project/src/**/reference_system*/",
    "project/src/**/*reference_system*",
    "project/src/**/*ReferenceSystem*",
    "project/src/udr/packages/[d]onor*/",
    "project/src/udr/packages/reference*/",
    "docs/documentation/compatibility-emulation/**",
    "docs/**/*[d]onor*",
    "docs/**/*[D]onor*",
    "docs/**/*reference-system*",
    "docs/**/*ReferenceSystem*",
    "project/docs/**/*[d]onor*",
    "project/docs/**/*[D]onor*",
    "project/docs/**/*reference-system*",
    "project/docs/**/*ReferenceSystem*",
    "project/tests/reference_regression/reference_release_acquisition/**",
    "!project/tests/reference_regression/reference_release_acquisition/**/regression/PUBLIC_REGRESSION_SCOPE.md",
    "!project/tests/reference_regression/reference_release_acquisition/**/regression/*_CANDIDATE.md",
    "!project/tests/reference_regression/reference_release_acquisition/**/regression/*_MANIFEST.csv",
    "!project/tests/reference_regression/reference_release_acquisition/**/regression/*_INDEX.csv",
    "project/tests/reference_regression/**/native_tool_harness/tools/**",
    "project/tests/reference_regression/firebird/original_firebird_qa/**",
)

FORBIDDEN_PUBLIC_PATH_TOKENS = (
    "d" "onor",
    "D" "onor",
    "D" "ONOR",
)

EXCLUDED_PATH_PREFIXES = (
    "docs/documentation/draft/",
)

ALLOWED_REFERENCE_ACQUISITION_FILES = (
    "PUBLIC_REGRESSION_SCOPE.md",
    "_CANDIDATE.md",
    "_MANIFEST.csv",
    "_INDEX.csv",
)

PUBLIC_STANDALONE_FORBIDDEN_TOKENS = (
    "SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS",
    "SB_BUILD_FIREBIRD_PARSER_WORKER",
    "SB_BUILD_SBU_FIREBIRD_PARSER_SUPPORT",
    "src/parsers/compatibility",
    "src/udr/packages/compatibility",
)

PUBLIC_RELEASE_REQUIRED_CMAKE_SNIPPETS = (
    'option(SB_BUILD_DATABASE_LIFECYCLE_TESTS "Build database lifecycle contract/registry CTest gates" OFF)',
)

PUBLIC_OUTPUT_STAGE_FORBIDDEN_TOKENS = (
    "SB_FBSQL_Parser",
    "SB_MYSQL_Parser",
    "SB_PGSQL_Parser",
    "SB_SQLITE_Parser",
    "SB_MARIADB_Parser",
    "SB_DUCKDB_Parser",
    "SB_CLICKHOUSE_Parser",
    "SB_TIDB_Parser",
    "SB_VITESS_Parser",
    "SB_COCKROACHDB_Parser",
    "SB_YUGABYTEDB_Parser",
    "SB_CASSANDRA_Parser",
    "SB_MONGODB_Parser",
    "SB_REDIS_Parser",
    "SB_OPENSEARCH_SQL_PPL_Parser",
    "SB_OPENSEARCH_Parser",
    "SB_NEO4J_Parser",
    "SB_INFLUXDB_Parser",
    "SB_MILVUS_Parser",
    "SB_DOLT_Parser",
    "SB_APACHE_IGNITE_Parser",
    "SB_TIKV_Parser",
    "SB_FOUNDATIONDB_Parser",
    "SB_IMMUDB_Parser",
    "SB_XTDB_Parser",
)


def fail(message: str) -> None:
    print(f"public_reference_material_boundary_gate=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def git_ls_files(repo_root: Path) -> list[str]:
    try:
        result = subprocess.run(
            ["git", "ls-files"],
            cwd=repo_root,
            check=True,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except subprocess.CalledProcessError as exc:
        fail(f"git_ls_files_failed:{exc.stderr.strip()}")
    return [line.strip() for line in result.stdout.splitlines() if line.strip()]


def is_excluded_path(path: str) -> bool:
    return any(path.startswith(prefix) for prefix in EXCLUDED_PATH_PREFIXES)


def blocked_public_path_terms(paths: list[str]) -> list[str]:
    blocked: list[str] = []
    for path in paths:
        if is_excluded_path(path):
            continue
        if any(token in path for token in FORBIDDEN_PUBLIC_PATH_TOKENS):
            blocked.append(path)
    return blocked


def is_allowed_reference_acquisition_metadata(path: str) -> bool:
    if not path.startswith("project/tests/reference_regression/reference_release_acquisition/"):
        return False
    if "/regression/" not in path:
        return False
    name = Path(path).name
    if name == "PUBLIC_REGRESSION_SCOPE.md":
        return True
    return any(name.endswith(suffix) for suffix in ALLOWED_REFERENCE_ACQUISITION_FILES[1:])


def blocked_external_payload_paths(paths: list[str]) -> list[str]:
    blocked: list[str] = []
    for path in paths:
        if "/native_tool_harness/tools/" in path:
            blocked.append(path)
        elif path.startswith("project/tests/reference_regression/firebird/original_firebird_qa/"):
            blocked.append(path)
        elif path.startswith("project/tests/reference_regression/reference_release_acquisition/") and not is_allowed_reference_acquisition_metadata(path):
            blocked.append(path)
    return blocked


def validate_gitignore(repo_root: Path) -> list[str]:
    gitignore = repo_root / ("." + "gitignore")
    if not gitignore.is_file():
        return ["missing " + "." + "gitignore"]
    text = gitignore.read_text(encoding="utf-8", errors="replace")
    missing = [pattern for pattern in REQUIRED_IGNORE_PATTERNS if pattern not in text]
    return [f"missing ignore pattern: {pattern}" for pattern in missing]


def validate_public_standalone_profile(repo_root: Path) -> list[str]:
    cmake_path = repo_root / "project" / "CMakeLists.txt"
    if not cmake_path.is_file():
        return ["missing project/CMakeLists.txt"]

    lines = cmake_path.read_text(encoding="utf-8", errors="replace").splitlines()
    in_block = False
    depth = 0
    block: list[tuple[int, str]] = []
    for line_number, line in enumerate(lines, start=1):
        stripped = line.strip()
        if stripped.startswith("if("):
            if stripped == "if(SB_BUILD_PUBLIC_STANDALONE_OUTPUT)":
                in_block = True
                depth = 1
                block = [(line_number, line)]
                continue
            if in_block:
                depth += 1
        elif stripped.startswith("endif()") and in_block:
            depth -= 1
            block.append((line_number, line))
            if depth == 0:
                break

        if in_block:
            block.append((line_number, line))

    if not block:
        return ["missing SB_BUILD_PUBLIC_STANDALONE_OUTPUT block"]

    errors: list[str] = []
    for line_number, line in block:
        for token in PUBLIC_STANDALONE_FORBIDDEN_TOKENS:
            if token in line:
                errors.append(
                    "public standalone profile enables private reference source "
                    f"token {token} at project/CMakeLists.txt:{line_number}"
                )
    return errors


def validate_public_release_option_defaults(repo_root: Path) -> list[str]:
    cmake_path = repo_root / "project" / "CMakeLists.txt"
    if not cmake_path.is_file():
        return ["missing project/CMakeLists.txt"]
    text = cmake_path.read_text(encoding="utf-8", errors="replace")
    return [
        f"missing public release CMake default: {snippet}"
        for snippet in PUBLIC_RELEASE_REQUIRED_CMAKE_SNIPPETS
        if snippet not in text
    ]


def validate_public_output_stage_gate(repo_root: Path) -> list[str]:
    gate_path = repo_root / "project" / "tools" / "release" / "public_output_stage_gate.py"
    branding_path = repo_root / "project" / "cmake" / "PublicBranding.cmake"
    errors: list[str] = []

    if not gate_path.is_file():
        errors.append("missing project/tools/release/public_output_stage_gate.py")
    else:
        gate_text = gate_path.read_text(encoding="utf-8", errors="replace")
        for token in PUBLIC_OUTPUT_STAGE_FORBIDDEN_TOKENS:
            if token in gate_text:
                errors.append(f"public output stage gate requires private parser artifact: {token}")

    if not branding_path.is_file():
        errors.append("missing project/cmake/PublicBranding.cmake")
    else:
        branding_text = branding_path.read_text(encoding="utf-8", errors="replace")
        forbidden_copy = '"${project_root}/config/templates"\n            "${SB_PUBLIC_ARTIFACT_ROOT}/etc/scratchbird"'
        if forbidden_copy in branding_text:
            errors.append("public output stage blindly copies all parser config templates")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--allow-driver-fixtures", action="store_true")
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    if not (repo_root / ("." + "git")).exists():
        fail(f"not_a_git_repository:{repo_root}")

    errors = validate_gitignore(repo_root)
    errors.extend(validate_public_standalone_profile(repo_root))
    errors.extend(validate_public_release_option_defaults(repo_root))
    errors.extend(validate_public_output_stage_gate(repo_root))
    tracked_paths = git_ls_files(repo_root)
    blocked_terms = blocked_public_path_terms(tracked_paths)
    errors.extend(f"tracked stale public path term: {path}" for path in blocked_terms)
    blocked_payloads = blocked_external_payload_paths(tracked_paths)
    errors.extend(f"tracked external reference payload: {path}" for path in blocked_payloads)

    if errors:
        for error in errors[:100]:
            print(error, file=sys.stderr)
        if len(errors) > 100:
            print(f"... {len(errors) - 100} additional errors", file=sys.stderr)
        return 1

    print("public_reference_material_boundary_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
