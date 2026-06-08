#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Standalone platform/toolchain matrix gate for CBQ-036."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys


START = "<!-- SB_PLATFORM_TOOLCHAIN_MATRIX_V1_START -->"
END = "<!-- SB_PLATFORM_TOOLCHAIN_MATRIX_V1_END -->"


def fail(message: str) -> int:
    print(f"PLATFORM_TOOLCHAIN_MATRIX: {message}", file=sys.stderr)
    return 1


def version_tuple(value: str) -> tuple[int, ...]:
    parts: list[int] = []
    for part in value.split("."):
        if not part:
            continue
        try:
            parts.append(int(part))
        except ValueError:
            break
    return tuple(parts)


def extract_matrix_block(text: str) -> str | None:
    start = text.find(START)
    end = text.find(END)
    if start == -1 or end == -1 or end <= start:
        return None
    return text[start + len(START):end]


def parse_metadata(path: Path) -> dict[str, str]:
    metadata: dict[str, str] = {}
    for raw in path.read_text(encoding="utf-8").splitlines():
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        if "=" not in raw:
            continue
        key, value = raw.split("=", 1)
        metadata[key.strip()] = value.strip()
    return metadata


def require_contains(text: str, token: str, context: str) -> int | None:
    if token not in text:
        return fail(f"{context} missing {token}")
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--build-root", required=True)
    parser.add_argument("--build-metadata", required=True)
    args = parser.parse_args()

    repo = Path(args.repo_root)
    build_root = Path(args.build_root)
    metadata_path = Path(args.build_metadata)
    matrix_path = repo / "project/cmake/SUPPORTED_PLATFORM_TOOLCHAIN_MATRIX.md"
    cmake_path = repo / "project/CMakeLists.txt"
    lifecycle_cmake = repo / "project/tests/database_lifecycle/CMakeLists.txt"

    for path in (matrix_path, cmake_path, lifecycle_cmake, metadata_path, build_root / "CMakeCache.txt"):
      if not path.exists():
        return fail(f"missing required input {path}")

    matrix_text = matrix_path.read_text(encoding="utf-8")
    matrix_block = extract_matrix_block(matrix_text)
    if matrix_block is None:
        return fail("matrix block markers missing or malformed")
    if "docs" "/execution-plans/" in matrix_text:
        return fail("source matrix must not depend on active execution_plan artifacts")

    cmake_text = cmake_path.read_text(encoding="utf-8")
    cmake_min_match = re.search(r"cmake_minimum_required\(VERSION\s+([0-9.]+)\)", cmake_text)
    if cmake_min_match is None:
        return fail("project/CMakeLists.txt missing cmake_minimum_required")
    cmake_min = cmake_min_match.group(1)
    if require_contains(matrix_block, f'cmake_minimum_required: "{cmake_min}"', "matrix"):
        return 1

    cxx_standard_match = re.search(r"set\(CMAKE_CXX_STANDARD\s+([0-9]+)\)", cmake_text)
    if cxx_standard_match is None:
        return fail("project/CMakeLists.txt missing CMAKE_CXX_STANDARD")
    cxx_standard = cxx_standard_match.group(1)
    if cxx_standard != "23":
        return fail(f"unexpected CMAKE_CXX_STANDARD {cxx_standard}")
    if require_contains(matrix_block, f'cxx_standard: "C++{cxx_standard}"', "matrix"):
        return 1
    for required in (
        "set(CMAKE_CXX_STANDARD_REQUIRED ON)",
        "set(CMAKE_CXX_EXTENSIONS OFF)",
        'set_property(CACHE SB_NONCLUSTER_ENGINE_PROFILE PROPERTY STRINGS',
        "option(SB_ENABLE_CLUSTER_PROVIDER",
        "option(SB_CLUSTER_PROVIDER_STUB",
        "SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY",
        "SB_CLUSTER_PROVIDER_STUB requires SB_ENABLE_CLUSTER_PROVIDER=ON",
    ):
        if required not in cmake_text:
            return fail(f"project/CMakeLists.txt missing {required}")

    for token in (
        'tested_cmake_family: "3.x"',
        'target_id: "linux-x86_64-glibc-core"',
        'os: "Linux"',
        'architecture: "x86_64"',
        'runtime: "glibc"',
        'family: "GCC"',
        'family: "Clang"',
        'minimum: "18"',
        'target_id: "windows-x86_64-msvc-core"',
        'target_id: "windows-x86_64-gnu-core"',
        'os: "Windows"',
        'runtime: "msvc"',
        'runtime: "ucrt64"',
        'family: "MSVC"',
        'target_id: "freebsd-x86_64-elf-core"',
        'os: "FreeBSD"',
        'runtime: "libc++"',
        'macos-first-release-out-of-scope',
        'unsupported_platform_fail_closed_before_configure_or_release_claim',
        'SB_DIAG_PLATFORM_UNSUPPORTED_FIRST_RELEASE',
        'driver_adaptor_toolchain_waiver_policy:',
        'missing_optional_toolchain: "deterministic_skip_with_reason"',
        'required_core_driver_toolchain: "must_build_or_fail_gate"',
        'execution_plan_artifacts: "not_ctest_inputs"',
        'cluster_compile_modes:',
        '"SB_ENABLE_CLUSTER_PROVIDER=OFF"',
        '"SB_ENABLE_CLUSTER_PROVIDER=ON"',
        '"SB_CLUSTER_PROVIDER_STUB=ON"',
        '"SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY=<path>"',
        "cluster_positive_implementation_outside_core",
        "no_execution_plan_ctest_dependency",
        '"release-complete"',
        '"bootstrap"',
        '"emergency"',
    ):
        if require_contains(matrix_block, token, "matrix"):
            return 1

    metadata = parse_metadata(metadata_path)
    required_metadata = (
        "CMAKE_VERSION",
        "CMAKE_CXX_COMPILER",
        "CMAKE_CXX_COMPILER_ID",
        "CMAKE_CXX_COMPILER_VERSION",
        "CMAKE_SYSTEM_NAME",
        "CMAKE_SYSTEM_PROCESSOR",
        "CMAKE_HOST_SYSTEM_NAME",
        "CMAKE_HOST_SYSTEM_PROCESSOR",
        "CMAKE_CXX_STANDARD",
        "SB_NONCLUSTER_ENGINE_PROFILE",
        "SB_ENABLE_CLUSTER_PROVIDER",
        "SB_CLUSTER_PROVIDER_STUB",
    )
    missing = [key for key in required_metadata if not metadata.get(key)]
    if missing:
        return fail(f"configured metadata missing {','.join(missing)}")

    if version_tuple(metadata["CMAKE_VERSION"]) < version_tuple(cmake_min):
        return fail(f"configured CMake {metadata['CMAKE_VERSION']} is below minimum {cmake_min}")
    if metadata["CMAKE_CXX_STANDARD"] != "23":
        return fail(f"configured CXX standard is {metadata['CMAKE_CXX_STANDARD']}, expected 23")
    system_name = metadata["CMAKE_SYSTEM_NAME"]
    processor = metadata["CMAKE_SYSTEM_PROCESSOR"]
    compiler_id = metadata["CMAKE_CXX_COMPILER_ID"]
    if processor not in {"x86_64", "amd64", "AMD64"}:
        return fail(
            "SB_DIAG_PLATFORM_UNSUPPORTED_FIRST_RELEASE: "
            f"first release supports x86_64 only, got {processor}"
        )
    if system_name == "Linux":
        if compiler_id not in {"GNU", "Clang"}:
            return fail(f"Linux beta matrix supports GNU/Clang, got {compiler_id}")
    elif system_name == "Windows":
        if compiler_id not in {"GNU", "MSVC"}:
            return fail(f"Windows beta matrix supports GNU/MSVC, got {compiler_id}")
    elif system_name == "FreeBSD":
        if compiler_id != "Clang":
            return fail(f"FreeBSD beta matrix supports Clang, got {compiler_id}")
    else:
        return fail(
            "SB_DIAG_PLATFORM_UNSUPPORTED_FIRST_RELEASE: "
            f"current configured system {system_name} is not in the first-release target matrix"
        )

    if metadata["SB_NONCLUSTER_ENGINE_PROFILE"] not in {"release-complete", "bootstrap", "emergency"}:
        return fail(f"unknown non-cluster profile {metadata['SB_NONCLUSTER_ENGINE_PROFILE']}")
    if metadata["SB_CLUSTER_PROVIDER_STUB"] == "ON" and metadata["SB_ENABLE_CLUSTER_PROVIDER"] != "ON":
        return fail("cluster stub metadata violates compile option boundary")

    lifecycle_cmake_text = lifecycle_cmake.read_text(encoding="utf-8")
    test_name = "database_lifecycle_platform_toolchain_matrix_conformance"
    if test_name not in lifecycle_cmake_text:
        return fail(f"{test_name} is not registered in database_lifecycle CMake")
    section_start = lifecycle_cmake_text.find(test_name)
    section = lifecycle_cmake_text[max(0, section_start - 500):section_start + 1200]
    if "docs" "/execution-plans" in section:
        return fail("platform/toolchain CTest registration depends on execution_plan artifacts")
    if "CBQ_GATE_PLATFORM_TOOLCHAIN_MATRIX" not in section:
        return fail("platform/toolchain CTest missing gate label")

    print(
        "platform_toolchain_matrix=passed "
        f"cmake={metadata['CMAKE_VERSION']} "
        f"compiler={metadata['CMAKE_CXX_COMPILER_ID']} "
        f"system={metadata['CMAKE_SYSTEM_NAME']} "
        f"arch={metadata['CMAKE_SYSTEM_PROCESSOR']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
