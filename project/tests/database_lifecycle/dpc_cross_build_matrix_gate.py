#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""DPC-069 cross-build matrix gate.

The required rows are embedded here so the CTest validates project build and
test surfaces without consuming execution_plan artifacts as runtime inputs.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import sys


DPC_CROSS_BUILD_MATRIX = "DPC_CROSS_BUILD_MATRIX"


@dataclass(frozen=True)
class MatrixRow:
    build_id: str
    cluster_mode: str
    build_type: str
    instrumentation_mode: str
    package_mode: bool
    cmake_options: tuple[str, ...]
    required_gate_tokens: tuple[str, ...]


REQUIRED_ROWS = (
    MatrixRow(
        "DPC_BUILD_DEBUG_NO_CLUSTER",
        "no-cluster",
        "Debug",
        "diagnostic",
        False,
        ("SB_ENABLE_CLUSTER_PROVIDER=OFF", "SB_CLUSTER_PROVIDER_STUB=OFF"),
        (
            "dpc_standalone_ctest_independence_gate",
            "dpc_negative_rejected_technique_drift_gate",
            "benchmark_clean_cmake_instrumentation_flags_gate",
        ),
    ),
    MatrixRow(
        "DPC_BUILD_RELEASE_NO_CLUSTER",
        "no-cluster",
        "Release",
        "normal",
        False,
        ("SB_ENABLE_CLUSTER_PROVIDER=OFF", "SB_CLUSTER_PROVIDER_STUB=OFF"),
        (
            "dpc_standalone_ctest_independence_gate",
            "dpc_negative_rejected_technique_drift_gate",
            "cluster_provider_no_cluster_error_vector_conformance",
        ),
    ),
    MatrixRow(
        "DPC_BUILD_BENCHMARK_CLEAN_NO_CLUSTER",
        "no-cluster",
        "Release",
        "benchmark-clean",
        False,
        (
            "SB_ENABLE_CLUSTER_PROVIDER=OFF",
            "SB_CLUSTER_PROVIDER_STUB=OFF",
            "SCRATCHBIRD_ENABLE_DEBUG_LOGS=OFF",
            "SCRATCHBIRD_ENABLE_HOTPATH_TRACE=OFF",
            "SCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE=OFF",
            "SCRATCHBIRD_ENABLE_PREPARED_TRACE=OFF",
        ),
        (
            "benchmark_clean_cmake_instrumentation_flags_gate",
            "dpc_standalone_ctest_independence_gate",
        ),
    ),
    MatrixRow(
        "DPC_BUILD_DEBUG_CLUSTER_STUB",
        "cluster-enabled-stub",
        "Debug",
        "diagnostic",
        False,
        ("SB_ENABLE_CLUSTER_PROVIDER=ON", "SB_CLUSTER_PROVIDER_STUB=ON"),
        (
            "agent_cluster_provider_build_matrix_gate",
            "sbsql_cluster_provider_stub_conformance",
            "sbsql_cluster_private_fail_closed_conformance",
        ),
    ),
    MatrixRow(
        "DPC_BUILD_RELEASE_CLUSTER_STUB",
        "cluster-enabled-stub",
        "Release",
        "normal",
        False,
        ("SB_ENABLE_CLUSTER_PROVIDER=ON", "SB_CLUSTER_PROVIDER_STUB=ON"),
        (
            "agent_cluster_provider_build_matrix_gate",
            "sbsql_cluster_provider_stub_conformance",
            "dpc_standalone_ctest_independence_gate",
        ),
    ),
    MatrixRow(
        "DPC_BUILD_PACKAGE_NO_CLUSTER",
        "no-cluster",
        "Release",
        "packaged-defaults",
        True,
        ("SB_ENABLE_CLUSTER_PROVIDER=OFF", "SB_CLUSTER_PROVIDER_STUB=OFF"),
        (
            "dpc_config_defaults_packaging_gate",
            "database_lifecycle_beta_package_smoke_gate",
            "cluster_provider_no_cluster_error_vector_conformance",
        ),
    ),
    MatrixRow(
        "DPC_BUILD_PACKAGE_CLUSTER_STUB",
        "cluster-enabled-stub",
        "Release",
        "packaged-defaults",
        True,
        ("SB_ENABLE_CLUSTER_PROVIDER=ON", "SB_CLUSTER_PROVIDER_STUB=ON"),
        (
            "dpc_config_defaults_packaging_gate",
            "database_lifecycle_beta_package_smoke_gate",
            "sbsql_cluster_provider_stub_conformance",
        ),
    ),
)


ADD_TEST_RE = re.compile(r"add_test\(\[=\[(?P<name>[^\]]+)\]=\](?P<body>.*?)\)", re.S)
LABEL_RE = re.compile(
    r"set_tests_properties\(\[=\[(?P<name>[^\]]+)\]=\]\s+PROPERTIES\s+"
    r".*?LABELS\s+\"(?P<labels>[^\"]+)\"",
    re.S,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--build-root", required=True)
    parser.add_argument("--build-metadata", required=True)
    return parser.parse_args()


def read_text(path: Path, errors: list[str]) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except FileNotFoundError:
        errors.append(f"missing required input: {path}")
    except OSError as exc:
        errors.append(f"failed to read {path}: {exc}")
    return ""


def parse_key_value_file(path: Path, errors: list[str]) -> dict[str, str]:
    values: dict[str, str] = {}
    for raw in read_text(path, errors).splitlines():
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        if "=" not in raw:
            continue
        key, value = raw.split("=", 1)
        values[key.strip()] = value.strip()
    return values


def parse_cache(path: Path, errors: list[str]) -> dict[str, str]:
    values: dict[str, str] = {}
    for raw in read_text(path, errors).splitlines():
        if not raw.strip() or raw.startswith(("//", "#")):
            continue
        if ":" not in raw or "=" not in raw:
            continue
        key_type, value = raw.split("=", 1)
        key, _cache_type = key_type.split(":", 1)
        values[key.strip()] = value.strip()
    return values


def parse_ctest_entries(build_root: Path, errors: list[str]) -> dict[str, str]:
    ctest_files = (
        build_root / "CTestTestfile.cmake",
        build_root / "tests/database_lifecycle/CTestTestfile.cmake",
        build_root / "tests/agents/CTestTestfile.cmake",
        build_root / "tests/sbsql_parser_worker/CTestTestfile.cmake",
    )
    entries: dict[str, str] = {}
    labels: dict[str, str] = {}
    for path in ctest_files:
        text = read_text(path, errors)
        for match in LABEL_RE.finditer(text):
            labels[match.group("name")] = match.group("labels")
        for match in ADD_TEST_RE.finditer(text):
            name = match.group("name")
            entries[name] = labels.get(name, "") + "\n" + match.group("body")
    return entries


def require_contains(text: str, token: str, context: str,
                     errors: list[str]) -> None:
    if token not in text:
        errors.append(f"{context} missing {token}")


def require_entry(entries: dict[str, str], name: str, required_tokens: tuple[str, ...],
                  errors: list[str]) -> None:
    body = entries.get(name)
    if body is None:
        errors.append(f"CTest metadata missing {name}")
        return
    for token in required_tokens:
        if token not in body:
            errors.append(f"{name} CTest metadata missing {token}")


def current_cluster_mode(values: dict[str, str]) -> str:
    if values.get("SB_ENABLE_CLUSTER_PROVIDER") == "ON":
        if values.get("SB_CLUSTER_PROVIDER_STUB") == "ON":
            return "cluster-enabled-stub"
        if values.get("SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY"):
            return "cluster-external"
        return "cluster-enabled-invalid"
    return "no-cluster"


def current_instrumentation_mode(values: dict[str, str]) -> str:
    build_type = values.get("CMAKE_BUILD_TYPE", "")
    flags = (
        "SCRATCHBIRD_ENABLE_DEBUG_LOGS",
        "SCRATCHBIRD_ENABLE_HOTPATH_TRACE",
        "SCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE",
        "SCRATCHBIRD_ENABLE_PREPARED_TRACE",
    )
    all_off = all(values.get(flag) == "OFF" for flag in flags)
    any_on = any(values.get(flag) == "ON" for flag in flags)
    if build_type == "Release" and all_off:
        return "benchmark-clean"
    if build_type == "Debug" or any_on:
        return "diagnostic"
    return "normal"


def compatible_instrumentation_modes(values: dict[str, str]) -> set[str]:
    mode = current_instrumentation_mode(values)
    if mode != "benchmark-clean":
        return {mode}

    # A Release build with all instrumentation compiled out is the same binary
    # shape used by the ordinary Release lane.  The benchmark-clean row adds
    # explicit evidence that every trace family is OFF and that the clean flag
    # gate is registered; it is not a distinct runtime ABI.
    return {"normal", "benchmark-clean"}


def validate_required_rows(errors: list[str]) -> None:
    expected = {
        "DPC_BUILD_DEBUG_NO_CLUSTER",
        "DPC_BUILD_RELEASE_NO_CLUSTER",
        "DPC_BUILD_BENCHMARK_CLEAN_NO_CLUSTER",
        "DPC_BUILD_DEBUG_CLUSTER_STUB",
        "DPC_BUILD_RELEASE_CLUSTER_STUB",
        "DPC_BUILD_PACKAGE_NO_CLUSTER",
        "DPC_BUILD_PACKAGE_CLUSTER_STUB",
    }
    observed = {row.build_id for row in REQUIRED_ROWS}
    if observed != expected:
        errors.append(f"embedded matrix rows drifted: {sorted(observed ^ expected)}")
    if len(REQUIRED_ROWS) != 7:
        errors.append(f"expected 7 embedded matrix rows, got {len(REQUIRED_ROWS)}")
    for row in REQUIRED_ROWS:
        if row.cluster_mode not in {"no-cluster", "cluster-enabled-stub"}:
            errors.append(f"{row.build_id} has unexpected cluster mode {row.cluster_mode}")
        if row.build_type not in {"Debug", "Release"}:
            errors.append(f"{row.build_id} has unexpected build type {row.build_type}")
        if not row.cmake_options or not row.required_gate_tokens:
            errors.append(f"{row.build_id} lacks options or gate tokens")
        if row.cluster_mode == "cluster-enabled-stub":
            for token in ("SB_ENABLE_CLUSTER_PROVIDER=ON", "SB_CLUSTER_PROVIDER_STUB=ON"):
                if token not in row.cmake_options:
                    errors.append(f"{row.build_id} missing {token}")
        if row.cluster_mode == "no-cluster":
            if "SB_ENABLE_CLUSTER_PROVIDER=OFF" not in row.cmake_options:
                errors.append(f"{row.build_id} missing no-cluster provider option")
        if row.instrumentation_mode == "benchmark-clean":
            for token in (
                "SCRATCHBIRD_ENABLE_DEBUG_LOGS=OFF",
                "SCRATCHBIRD_ENABLE_HOTPATH_TRACE=OFF",
                "SCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE=OFF",
                "SCRATCHBIRD_ENABLE_PREPARED_TRACE=OFF",
            ):
                if token not in row.cmake_options:
                    errors.append(f"{row.build_id} missing benchmark-clean {token}")


def validate_source_surfaces(repo_root: Path, errors: list[str]) -> None:
    project_cmake = read_text(repo_root / "project/CMakeLists.txt", errors)
    lifecycle_cmake = read_text(
        repo_root / "project/tests/database_lifecycle/CMakeLists.txt", errors)
    agents_cmake = read_text(repo_root / "project/tests/agents/CMakeLists.txt", errors)
    sbsql_cmake = read_text(
        repo_root / "project/tests/sbsql_parser_worker/CMakeLists.txt", errors)
    internal_api_cmake = read_text(
        repo_root / "project/src/engine/internal_api/CMakeLists.txt", errors)
    sblr_cmake = read_text(repo_root / "project/src/engine/sblr/CMakeLists.txt", errors)
    no_cluster_cmake = read_text(
        repo_root / "project/src/cluster_provider/CMakeLists.txt", errors)
    stub_cmake = read_text(
        repo_root / "project/src/cluster_provider_stub/CMakeLists.txt", errors)
    cmake_readme = read_text(repo_root / "project/cmake/README.md", errors)
    benchmark_gate = read_text(
        repo_root / "project/cmake/BenchmarkCleanInstrumentationFlagsGate.cmake", errors)
    config_template = read_text(
        repo_root / "project/cmake/ScratchBirdEngineConfig.cmake.in", errors)
    agent_gate = read_text(
        repo_root / "project/tests/agents/agent_cluster_provider_build_matrix_gate.cpp",
        errors,
    )

    for token in (
        "option(SB_ENABLE_CLUSTER_PROVIDER",
        "option(SB_CLUSTER_PROVIDER_STUB",
        "SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY",
        "SB_CLUSTER_PROVIDER_EXTERNAL_INCLUDE_DIR",
        "SB_CLUSTER_PROVIDER_STUB AND NOT SB_ENABLE_CLUSTER_PROVIDER",
        "SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY AND NOT SB_ENABLE_CLUSTER_PROVIDER",
        "SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY AND SB_CLUSTER_PROVIDER_STUB",
        "SCRATCHBIRD_ENABLE_DEBUG_LOGS",
        "SCRATCHBIRD_ENABLE_HOTPATH_TRACE",
        "SCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE",
        "SCRATCHBIRD_ENABLE_PREPARED_TRACE",
        "configure_package_config_file",
        "ScratchBirdEngineConfig.cmake.in",
        "install(EXPORT ScratchBirdEngineTargets",
    ):
        require_contains(project_cmake, token, "project/CMakeLists.txt", errors)

    for token in (
        "SCRATCHBIRD_CLUSTER_PROVIDER_EXTERNAL=1",
        "src/cluster_provider_stub",
        "src/cluster_provider",
        "requires SB_CLUSTER_PROVIDER_STUB=ON or SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY",
    ):
        if token not in internal_api_cmake and token not in sblr_cmake:
            errors.append(f"cluster provider target selection missing {token}")

    require_contains(no_cluster_cmake, "SCRATCHBIRD_CLUSTER_PROVIDER_NO_CLUSTER=1",
                     "no-cluster provider CMake", errors)
    require_contains(stub_cmake, "SCRATCHBIRD_CLUSTER_PROVIDER_STUB=1",
                     "stub provider CMake", errors)
    require_contains(benchmark_gate, "DPC_BENCHMARK_CLEAN_FLAGS_GATE",
                     "benchmark-clean CMake gate", errors)
    require_contains(benchmark_gate, "CMakeCache.txt", "benchmark-clean CMake gate", errors)
    require_contains(cmake_readme, "CMake Support", "project/cmake README", errors)
    require_contains(config_template, "@PACKAGE_INIT@", "package config template", errors)
    require_contains(config_template, "ScratchBirdEngineTargets.cmake",
                     "package config template", errors)

    for token in (
        "dpc_cross_build_matrix_gate",
        "DPC-069",
        "DPC_P6_CROSS_BUILD_READY",
        "DPC_AUDIT_CROSS_BUILD",
        "DPC_DEP_CROSS_BUILD",
        "cross_build_matrix",
        "mga_transaction_regression",
    ):
        require_contains(lifecycle_cmake, token, "database_lifecycle CMake", errors)
    require_contains(lifecycle_cmake, "dpc_config_defaults_packaging_gate",
                     "database_lifecycle CMake", errors)
    require_contains(lifecycle_cmake, "DPC-068", "database_lifecycle CMake", errors)
    require_contains(agents_cmake, "agent_cluster_provider_build_matrix_gate",
                     "agents CMake", errors)
    require_contains(sbsql_cmake, "sbsql_cluster_provider_stub_conformance",
                     "SBSQL CMake", errors)
    require_contains(sbsql_cmake, "cluster_provider_no_cluster_error_vector_conformance",
                     "SBSQL CMake", errors)
    require_contains(sbsql_cmake, "sbsql_cluster_private_fail_closed_conformance",
                     "SBSQL CMake", errors)

    for token in (
        "SCRATCHBIRD_CLUSTER_PROVIDER_NO_CLUSTER",
        "SCRATCHBIRD_CLUSTER_PROVIDER_STUB",
        "SCRATCHBIRD_CLUSTER_PROVIDER_EXTERNAL",
        "ClusterProviderSupportsExecution",
        "RouteAgentClusterProviderBoundary",
    ):
        require_contains(agent_gate, token, "agent cluster matrix gate", errors)


def validate_ctest_metadata(entries: dict[str, str], values: dict[str, str],
                            errors: list[str]) -> None:
    require_entry(entries, "benchmark_clean_cmake_instrumentation_flags_gate",
                  ("DPC-001", "benchmark_clean", "cmake_config"), errors)
    require_entry(entries, "dpc_config_defaults_packaging_gate",
                  ("DPC-068", "DPC_P6_CONFIG_PACKAGING_READY",
                   "DPC_AUDIT_CONFIG_PACKAGING", "DPC_DEP_CONFIG_PACKAGING"), errors)
    require_entry(entries, "dpc_standalone_ctest_independence_gate",
                  ("DPC-062", "DPC_P6_STANDALONE_CTEST_READY"), errors)
    require_entry(entries, "dpc_negative_rejected_technique_drift_gate",
                  ("DPC-067", "DPC_P6_NEGATIVE_DRIFT_READY"), errors)
    require_entry(entries, "agent_cluster_provider_build_matrix_gate",
                  ("cluster_provider_build_matrix", "agents"), errors)
    require_entry(entries, "sbsql_cluster_private_fail_closed_conformance",
                  ("SBSFC-025", "sbsql_parser_worker"), errors)

    cluster_mode = current_cluster_mode(values)
    if cluster_mode == "cluster-enabled-stub":
        require_entry(entries, "sbsql_cluster_provider_stub_conformance",
                      ("sbsql_cluster_provider_conformance", "sbsql_parser_worker"),
                      errors)
    elif cluster_mode == "no-cluster":
        require_entry(entries, "cluster_provider_no_cluster_error_vector_conformance",
                      ("sbsql_cluster_provider_conformance", "sbsql_parser_worker"),
                      errors)
    else:
        errors.append(f"current build has unsupported cluster mode {cluster_mode}")


def validate_no_execution_plan_runtime_paths(repo_root: Path, build_root: Path,
                                       metadata_path: Path,
                                       entries: dict[str, str],
                                       errors: list[str]) -> None:
    active_fragment = "/".join(("docs", "execution-plans"))
    completed_fragment = "/".join(("docs", "completed-execution-plans"))
    for label, path in (
        ("repo root", repo_root),
        ("build root", build_root),
        ("build metadata", metadata_path),
    ):
        normalized = path.as_posix()
        if active_fragment in normalized or completed_fragment in normalized:
            errors.append(f"{label} points into a execution_plan tree: {path}")
    command = entries.get("dpc_cross_build_matrix_gate", "")
    if active_fragment in command or completed_fragment in command:
        errors.append("dpc_cross_build_matrix_gate CTest command uses a execution_plan tree")


def validate_current_build(values: dict[str, str], cache_values: dict[str, str],
                           errors: list[str]) -> None:
    required_keys = (
        "CMAKE_BUILD_TYPE",
        "SB_ENABLE_CLUSTER_PROVIDER",
        "SB_CLUSTER_PROVIDER_STUB",
        "SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY",
        "SB_CLUSTER_PROVIDER_EXTERNAL_INCLUDE_DIR",
        "SCRATCHBIRD_ENABLE_DEBUG_LOGS",
        "SCRATCHBIRD_ENABLE_HOTPATH_TRACE",
        "SCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE",
        "SCRATCHBIRD_ENABLE_PREPARED_TRACE",
    )
    for key in required_keys:
        if key not in values:
            errors.append(f"build metadata missing {key}")
        if key not in cache_values:
            errors.append(f"CMakeCache missing {key}")
        elif key in values and values[key] != cache_values[key]:
            errors.append(f"metadata/cache mismatch for {key}: {values[key]} != {cache_values[key]}")

    if values.get("SB_CLUSTER_PROVIDER_STUB") == "ON" and (
            values.get("SB_ENABLE_CLUSTER_PROVIDER") != "ON"):
        errors.append("current cache enables cluster stub without cluster provider")
    if values.get("SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY") and (
            values.get("SB_ENABLE_CLUSTER_PROVIDER") != "ON"):
        errors.append("current cache configures external provider without cluster provider")
    if values.get("SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY") and (
            values.get("SB_CLUSTER_PROVIDER_STUB") == "ON"):
        errors.append("current cache configures both external provider and stub")

    cluster_mode = current_cluster_mode(values)
    build_type = values.get("CMAKE_BUILD_TYPE", "")
    instrumentation = current_instrumentation_mode(values)
    compatible_modes = compatible_instrumentation_modes(values)
    matching_rows = [
        row for row in REQUIRED_ROWS
        if row.cluster_mode == cluster_mode
        and row.build_type == build_type
        and row.instrumentation_mode in compatible_modes
    ]
    if not matching_rows:
        errors.append(
            "current configured build does not match an embedded DPC-069 row: "
            f"cluster_mode={cluster_mode} build_type={build_type} "
            f"instrumentation={instrumentation} "
            f"compatible_modes={sorted(compatible_modes)}"
        )


def main() -> int:
    args = parse_args()
    repo_root = Path(args.repo_root).resolve()
    build_root = Path(args.build_root).resolve()
    metadata_path = Path(args.build_metadata).resolve()
    errors: list[str] = []

    validate_required_rows(errors)
    validate_source_surfaces(repo_root, errors)
    metadata = parse_key_value_file(metadata_path, errors)
    cache_values = parse_cache(build_root / "CMakeCache.txt", errors)
    validate_current_build(metadata, cache_values, errors)
    entries = parse_ctest_entries(build_root, errors)
    validate_no_execution_plan_runtime_paths(repo_root, build_root, metadata_path, entries, errors)
    validate_ctest_metadata(entries, metadata, errors)

    if errors:
        for error in errors:
            print(f"DPC-069 ERROR: {error}", file=sys.stderr)
        return 1

    print(
        "dpc_cross_build_matrix_gate=passed "
        f"rows={len(REQUIRED_ROWS)} "
        f"current_cluster={current_cluster_mode(metadata)} "
        f"current_build_type={metadata.get('CMAKE_BUILD_TYPE', '')} "
        f"current_instrumentation={current_instrumentation_mode(metadata)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
