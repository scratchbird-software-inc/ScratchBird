#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate first-release public platform and provider-mode proof inputs."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
from typing import Any


START = "<!-- SB_PLATFORM_TOOLCHAIN_MATRIX_V1_START -->"
END = "<!-- SB_PLATFORM_TOOLCHAIN_MATRIX_V1_END -->"

REQUIRED_PLATFORMS: dict[str, dict[str, Any]] = {
    "linux": {
        "target_id": "linux-x86_64-glibc-core",
        "os": "Linux",
        "runtime": "glibc",
        "host_systems": {"Linux"},
        "compiler_families": {"GNU": "13", "Clang": "18"},
        "matrix_compilers": (("GCC", "13"), ("Clang", "18")),
        "doc_tokens": (
            "Ubuntu 24.04 LTS x86_64",
            "gcc-13",
            "clang-18",
            "SB_BUILD_PUBLIC_RELEASE_CORRECTNESS=ON",
            "SB_ENABLE_CLUSTER_PROVIDER=OFF",
            "SB_LLVM_LINK_MODE=dynamic",
            "ctest --test-dir",
            "cluster execution succeeds without the external cluster provider",
        ),
    },
    "windows": {
        "target_id": "windows-x86_64-gnu-core",
        "os": "Windows",
        "runtime": "ucrt64",
        "host_systems": {"Windows"},
        "compiler_families": {"GNU": "15"},
        "matrix_compilers": (("GCC", "15"),),
        "doc_tokens": (
            "Windows 11 x64",
            "Windows Server 2022/2025 x64",
            "MSYS2 UCRT64",
            "GNU C++",
            "gcc 15",
            "SB_BUILD_PUBLIC_RELEASE_CORRECTNESS=ON",
            "SB_ENABLE_CLUSTER_PROVIDER=OFF",
            "SB_LLVM_LINK_MODE=dynamic",
            "ctest --test-dir",
            "cluster execution succeeds without the external cluster provider",
            "Win32 is not a supported release target",
            "x64 tools, x64 dependencies, x64 Python",
        ),
    },
    "freebsd": {
        "target_id": "freebsd-x86_64-elf-core",
        "os": "FreeBSD",
        "runtime": "libc++",
        "host_systems": {"FreeBSD"},
        "compiler_families": {"Clang": "18"},
        "matrix_compilers": (("Clang", "18"),),
        "doc_tokens": (
            "FreeBSD 14.x x86_64",
            "llvm18",
            "SB_BUILD_PUBLIC_RELEASE_CORRECTNESS=ON",
            "SB_ENABLE_CLUSTER_PROVIDER=OFF",
            "SB_LLVM_LINK_MODE=dynamic",
            "ctest --test-dir",
            "no native FreeBSD runner produced passing artifacts",
            "cluster execution succeeds without the external cluster provider",
        ),
    },
}

ARCH_ALIASES = {"x86_64", "amd64", "AMD64"}
WINDOWS_32BIT_PROCESSORS = {"x86", "X86", "Win32", "win32", "I386", "i386", "i686"}

REQUIRED_LAYOUT_EXCLUSIONS = {
    "listener",
    "parser",
    "manager",
    "driver",
    "udr",
    "cluster_provider",
}

FORBIDDEN_REFERENCE_FRAGMENTS = (
    "docs" + "/" + "execution-plans",
    "docs" + "/" + "completed-execution-plans",
    "docs" + "/" + "findings",
    "." + "git",
    "/" + "home" + "/" + "dcalford",
    "ScratchBird" + "-Private",
)


def fail(message: str) -> None:
    print(f"public_platform_matrix_gate=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def version_tuple(value: str) -> tuple[int, ...]:
    pieces: list[int] = []
    for part in re.split(r"[.+-]", value):
        if not part:
            continue
        if not part.isdigit():
            break
        pieces.append(int(part))
    return tuple(pieces)


def rel(path: Path, repo_root: Path) -> str:
    return path.relative_to(repo_root).as_posix()


def require_file(path: Path, repo_root: Path) -> str:
    if not path.exists() or not path.is_file():
        fail(f"required_file_missing:{rel(path, repo_root)}")
    return path.read_text(encoding="utf-8")


def reject_private_reference(value: str, context: str) -> None:
    if Path(value).is_absolute():
        fail(f"absolute_path_recorded:{context}:{value}")
    for fragment in FORBIDDEN_REFERENCE_FRAGMENTS:
        if fragment in value:
            fail(f"private_reference_recorded:{context}:{value}")


def require_contains(text: str, token: str, context: str) -> None:
    if token not in text:
        fail(f"{context}_missing:{token}")


def extract_matrix_block(text: str) -> str:
    start = text.find(START)
    end = text.find(END)
    if start == -1 or end == -1 or end <= start:
        fail("matrix_block_markers_missing")
    return text[start + len(START):end]


def matrix_target_block(matrix_block: str, target_id: str) -> str:
    needle = f'target_id: "{target_id}"'
    start = matrix_block.find(needle)
    if start == -1:
        fail(f"matrix_target_missing:{target_id}")
    next_target = matrix_block.find("\n  - target_id:", start + len(needle))
    next_section = matrix_block.find("\n\nexplicit_waivers_and_skips:", start)
    candidates = [value for value in (next_target, next_section) if value != -1]
    end = min(candidates) if candidates else len(matrix_block)
    return matrix_block[start:end]


def load_json(path: Path, repo_root: Path) -> dict[str, Any]:
    try:
        return json.loads(require_file(path, repo_root))
    except json.JSONDecodeError as exc:
        fail(f"json_invalid:{rel(path, repo_root)}:{exc}")


def check_project_cmake(repo_root: Path, project_root: Path) -> None:
    cmake_text = require_file(project_root / "CMakeLists.txt", repo_root)
    for token in (
        "cmake_minimum_required(VERSION 3.25)",
        "set(CMAKE_CXX_STANDARD 23)",
        "set(CMAKE_CXX_STANDARD_REQUIRED ON)",
        "set(CMAKE_CXX_EXTENSIONS OFF)",
        "SB_BUILD_PUBLIC_RELEASE_CORRECTNESS",
        "SB_ENABLE_CLUSTER_PROVIDER",
        "SB_CLUSTER_PROVIDER_STUB",
        "SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY",
        "SB_CLUSTER_PROVIDER_STUB requires SB_ENABLE_CLUSTER_PROVIDER=ON",
        "SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY requires SB_ENABLE_CLUSTER_PROVIDER=ON",
        "Choose either SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY or SB_CLUSTER_PROVIDER_STUB, not both",
        "tests/release",
        "PUBLIC_RELEASE_PLATFORM_MATRIX",
    ):
        require_contains(cmake_text, token, "project_cmake")


def check_release_ctest(repo_root: Path, project_root: Path) -> None:
    ctest_text = require_file(project_root / "tests" / "release" / "CMakeLists.txt", repo_root)
    for token in (
        "PUBLIC_PLATFORM_MATRIX_GATE",
        "public_platform_matrix_gate",
        "public_platform_matrix_gate.py",
        "PCR-GATE-109",
        "PCR-109",
        "public_release_correctness",
        "cluster_boundary",
        "fail_closed",
        "--configured-system-name",
        "--configured-cxx-compiler-id",
    ):
        require_contains(ctest_text, token, "release_ctest")


def check_matrix(repo_root: Path, project_root: Path) -> dict[str, Any]:
    matrix_path = project_root / "cmake" / "SUPPORTED_PLATFORM_TOOLCHAIN_MATRIX.md"
    matrix_text = require_file(matrix_path, repo_root)
    for fragment in FORBIDDEN_REFERENCE_FRAGMENTS:
        if fragment in matrix_text:
            fail(f"matrix_private_reference:{fragment}")
    matrix_block = extract_matrix_block(matrix_text)
    for token in (
        "matrix_version: 1",
        'cmake_minimum_required: "3.25"',
        'cxx_standard: "C++23"',
        'cxx_extensions: "off"',
        "beta_supported_targets:",
        "unsupported_platform_diagnostics:",
        "SB_DIAG_PLATFORM_UNSUPPORTED_FIRST_RELEASE",
        "macos-first-release-out-of-scope",
        "unsupported_platform_fail_closed_before_configure_or_release_claim",
        "cluster_compile_modes:",
        'mode: "noncluster"',
        '"SB_ENABLE_CLUSTER_PROVIDER=OFF"',
        '"SB_CLUSTER_PROVIDER_STUB=OFF"',
        'mode: "cluster_stub_boundary"',
        '"SB_CLUSTER_PROVIDER_STUB=ON"',
        'mode: "cluster_external_boundary"',
        '"SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY=<path>"',
        "routes_to_external_provider_abi_without_claiming_closed_source_cluster_implementation_in_core",
        "cluster_positive_implementation_outside_core",
        "no_execution_plan_ctest_dependency",
    ):
        require_contains(matrix_block, token, "matrix")

    platform_records: list[dict[str, Any]] = []
    for platform_id, spec in REQUIRED_PLATFORMS.items():
        block = matrix_target_block(matrix_block, spec["target_id"])
        for token in (
            f'os: "{spec["os"]}"',
            'architecture: "x86_64"',
            f'runtime: "{spec["runtime"]}"',
            f'release_layout: "release/{platform_id}/ENGINE_BINARY_LAYOUT.json"',
            f'build_requirements: "docs/build_requirements/{platform_id}/README.md"',
            '"noncluster_release_complete"',
            '"noncluster_bootstrap"',
            '"noncluster_emergency"',
        ):
            require_contains(block, token, f"matrix_{platform_id}")
        for family, minimum in spec["matrix_compilers"]:
            require_contains(block, f'family: "{family}"', f"matrix_{platform_id}")
            require_contains(block, f'minimum: "{minimum}"', f"matrix_{platform_id}")
        platform_records.append(
            {
                "platform": platform_id,
                "target_id": spec["target_id"],
                "matrix_status": "declared",
            }
        )
        if platform_id == "windows":
            for token in (
                "unsupported_architecture_aliases:",
                '"Win32"',
                '"x86"',
                '"i386"',
                '"i686"',
                'unsupported_architecture_behavior: "windows_x64_only_no_win32_release_target"',
            ):
                require_contains(block, token, "matrix_windows_x64_only")
    if re.search(r'target_id:\s*"windows-(?!x86_64)[^"]*"', matrix_block):
        fail("windows_non_x64_target_declared")
    if "target_id: \"macos" in matrix_block.lower() or "target_id: \"darwin" in matrix_block.lower():
        fail("macos_must_not_be_declared_first_release_target")
    for token in (
        "win32-first-release-out-of-scope",
        "Windows 32-bit / Win32 / x86",
        "windows_x64_only_no_win32_release_target",
        "Windows 32-bit",
        "x86 Windows",
    ):
        require_contains(matrix_block, token, "matrix_windows_x64_boundary")
    return {"path": rel(matrix_path, repo_root), "platforms": platform_records}


def check_build_requirements(repo_root: Path) -> list[dict[str, Any]]:
    docs_root = repo_root / "docs" / "build_requirements"
    root_text = require_file(docs_root / "README.md", repo_root)
    for token in (
        "Linux x86_64, Ubuntu 24.04 LTS",
        "Fully proven first target",
        "Windows x64, Windows 11 or Windows Server 2022/2025",
        "Target platform pending native CI/runtime proof",
        "FreeBSD x86_64, FreeBSD 14.x",
        "Target platform pending native runner proof",
        "macOS | Out of scope for first public release",
        "All support-eligible platforms must provide before support is claimed",
        "Every support-eligible platform must prove before support is claimed",
        "CMake 3.25 minimum",
        "C++23 compiler",
        "Python 3.11 or newer",
        "LLVM 23 or newer",
        "SB_LLVM_LINK_MODE=dynamic",
        "SB_ENABLE_CLUSTER_PROVIDER=OFF",
        "SB_CLUSTER_PROVIDER_STUB=ON",
        "External cluster-provider proof only when the closed cluster library is",
    ):
        require_contains(root_text, token, "build_requirements_root")
    for forbidden in (
        "Supported target with CI/runtime proof",
        "Supported only after runner proof",
        "supported target with CI/runtime proof",
        "supported only after runner proof",
        "All supported platforms must provide",
        "Every supported platform must prove",
    ):
        if forbidden in root_text:
            fail(f"build_requirements_platform_overclaim:{forbidden}")

    records: list[dict[str, Any]] = []
    for platform_id, spec in REQUIRED_PLATFORMS.items():
        path = docs_root / platform_id / "README.md"
        text = require_file(path, repo_root)
        for token in spec["doc_tokens"]:
            require_contains(text, token, f"build_requirements_{platform_id}")
        records.append(
            {
                "platform": platform_id,
                "path": rel(path, repo_root),
                "status": "public_requirements_declared",
            }
        )
    return records


def check_release_layouts(repo_root: Path) -> list[dict[str, Any]]:
    release_root = repo_root / "release"
    layouts: list[dict[str, Any]] = []
    for platform_id in REQUIRED_PLATFORMS:
        path = release_root / platform_id / "ENGINE_BINARY_LAYOUT.json"
        payload = load_json(path, repo_root)
        if payload.get("platform") != platform_id:
            fail(f"layout_platform_mismatch:{platform_id}")
        if payload.get("status") != "layout_definition":
            fail(f"layout_bad_status:{platform_id}")
        if payload.get("cluster_execution") != "external_provider_only":
            fail(f"layout_cluster_boundary_overclaim:{platform_id}")
        roots = payload.get("engine_payload_roots", [])
        if not isinstance(roots, list) or not roots:
            fail(f"layout_engine_roots_missing:{platform_id}")
        for root in roots:
            if not isinstance(root, str):
                fail(f"layout_engine_root_bad_type:{platform_id}")
            reject_private_reference(root, f"layout_{platform_id}")
            if not root.startswith(f"release/{platform_id}/"):
                fail(f"layout_engine_root_outside_platform:{platform_id}:{root}")
        setup_root = payload.get("setup_payload_root", "")
        if not isinstance(setup_root, str) or not setup_root.startswith(f"release/{platform_id}/"):
            fail(f"layout_setup_root_outside_platform:{platform_id}")
        reject_private_reference(setup_root, f"layout_{platform_id}")
        debug_symbol_root = payload.get("debug_symbol_payload_root", "")
        if (
            not isinstance(debug_symbol_root, str)
            or not debug_symbol_root.startswith(f"release/{platform_id}/")
        ):
            fail(f"layout_debug_symbol_root_outside_platform:{platform_id}")
        reject_private_reference(debug_symbol_root, f"layout_{platform_id}")
        roles = payload.get("allowed_binary_roles", [])
        if not roles or any(not isinstance(role, str) or not role.startswith("engine_") for role in roles):
            fail(f"layout_non_engine_allowed_role:{platform_id}")
        debug_symbol_roles = payload.get("allowed_debug_symbol_roles", [])
        if (
            not debug_symbol_roles
            or any(
                not isinstance(role, str)
                or not role.startswith("engine_")
                or "debug" not in role
                for role in debug_symbol_roles
            )
        ):
            fail(f"layout_debug_symbol_roles_missing:{platform_id}")
        if (
            payload.get("debug_symbol_package_policy")
            != "separate_debug_symbols_required_for_release_artifacts"
        ):
            fail(f"layout_debug_symbol_policy_missing:{platform_id}")
        exclusions = set(payload.get("excluded_binary_families", []))
        missing_exclusions = sorted(REQUIRED_LAYOUT_EXCLUSIONS - exclusions)
        if missing_exclusions:
            fail(f"layout_missing_exclusions:{platform_id}:{','.join(missing_exclusions)}")
        layouts.append(
            {
                "platform": platform_id,
                "path": rel(path, repo_root),
                "status": "engine_binary_layout_declared",
                "cluster_execution": payload["cluster_execution"],
                "debug_symbol_payload_root": debug_symbol_root,
                "debug_symbol_package_policy": payload["debug_symbol_package_policy"],
            }
        )
    for unsupported in ("macos", "darwin"):
        if (release_root / unsupported).exists():
            fail(f"unsupported_platform_layout_present:{unsupported}")
    return layouts


def configured_windows_x86_hard_fail(system_name: str, processor: str) -> bool:
    return system_name == "Windows" and processor in WINDOWS_32BIT_PROCESSORS


def build_windows_x64_only_negative_proofs() -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for processor in sorted(WINDOWS_32BIT_PROCESSORS):
        if not configured_windows_x86_hard_fail("Windows", processor):
            fail(f"windows_x86_negative_proof_not_rejected:{processor}")
        rows.append(
            {
                "system": "Windows",
                "processor": processor,
                "expected_gate_behavior": "hard_fail",
                "diagnostic": "SB_DIAG_PLATFORM_UNSUPPORTED_FIRST_RELEASE",
                "reason": "windows_x64_only_no_win32_release_target",
            }
        )
    for processor in sorted(ARCH_ALIASES):
        if configured_windows_x86_hard_fail("Windows", processor):
            fail(f"windows_x64_alias_misclassified:{processor}")
    return rows


def host_platform_id(system_name: str) -> str | None:
    for platform_id, spec in REQUIRED_PLATFORMS.items():
        if system_name in spec["host_systems"]:
            return platform_id
    return None


def check_configured_host(args: argparse.Namespace) -> dict[str, Any]:
    system_name = args.configured_system_name
    processor = args.configured_system_processor
    compiler_id = args.configured_cxx_compiler_id
    compiler_version = args.configured_cxx_compiler_version
    cmake_version = args.configured_cmake_version
    cxx_standard = args.configured_cxx_standard

    if version_tuple(cmake_version) < version_tuple("3.25"):
        fail(f"configured_cmake_below_minimum:{cmake_version}")
    if cxx_standard != "23":
        fail(f"configured_cxx_standard_mismatch:{cxx_standard}")
    if configured_windows_x86_hard_fail(system_name, processor):
        fail(f"configured_windows_x86_not_supported:{processor}:windows_x64_only_no_win32_release_target")
    if processor not in ARCH_ALIASES:
        return {
            "system": system_name,
            "processor": processor,
            "status": "unsupported_fail_closed",
            "diagnostic": "SB_DIAG_PLATFORM_UNSUPPORTED_FIRST_RELEASE",
            "reason": "first_release_x86_64_only",
        }

    platform_id = host_platform_id(system_name)
    if platform_id is None:
        return {
            "system": system_name,
            "processor": processor,
            "status": "unsupported_fail_closed",
            "diagnostic": "SB_DIAG_PLATFORM_UNSUPPORTED_FIRST_RELEASE",
            "reason": "platform_not_listed_in_beta_supported_targets",
        }

    min_versions = REQUIRED_PLATFORMS[platform_id]["compiler_families"]
    if compiler_id not in min_versions:
        fail(f"configured_compiler_not_in_matrix:{platform_id}:{compiler_id}")
    minimum = min_versions[compiler_id]
    if version_tuple(compiler_version) < version_tuple(minimum):
        fail(f"configured_compiler_below_minimum:{platform_id}:{compiler_id}:{compiler_version}< {minimum}")

    cluster_mode = {
        "SB_ENABLE_CLUSTER_PROVIDER": args.cluster_provider_enabled,
        "SB_CLUSTER_PROVIDER_STUB": args.cluster_provider_stub,
        "SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY": bool(args.cluster_provider_external_library),
    }
    if args.cluster_provider_stub == "ON" and args.cluster_provider_enabled != "ON":
        fail("configured_cluster_stub_without_provider")
    if args.cluster_provider_external_library and args.cluster_provider_enabled != "ON":
        fail("configured_external_cluster_without_provider")
    if args.cluster_provider_external_library and args.cluster_provider_stub == "ON":
        fail("configured_external_cluster_and_stub")

    return {
        "platform": platform_id,
        "system": system_name,
        "processor": processor,
        "compiler_id": compiler_id,
        "compiler_version": compiler_version,
        "cmake_version": cmake_version,
        "cxx_standard": cxx_standard,
        "cluster_provider_mode": cluster_mode,
        "status": "native_host_profile_validated",
    }


def build_evidence(repo_root: Path, project_root: Path, args: argparse.Namespace) -> dict[str, Any]:
    check_project_cmake(repo_root, project_root)
    check_release_ctest(repo_root, project_root)
    matrix = check_matrix(repo_root, project_root)
    build_requirements = check_build_requirements(repo_root)
    release_layouts = check_release_layouts(repo_root)
    negative_platform_proofs = build_windows_x64_only_negative_proofs()
    host = check_configured_host(args)

    nonhost_records: list[dict[str, str]] = []
    host_platform = host.get("platform")
    for platform_id in REQUIRED_PLATFORMS:
        if platform_id == host_platform:
            continue
        nonhost_records.append(
            {
                "platform": platform_id,
                "status": "native_runner_required",
                "diagnostic": "SB_DIAG_PLATFORM_NATIVE_PROOF_REQUIRED",
                "behavior": "no_cross_platform_support_claim_from_current_host",
            }
        )

    evidence: dict[str, Any] = {
        "schema_version": 1,
        "policy": {
            "public_tree_inputs_only": True,
            "private_docs_required": False,
            "git_history_required": False,
            "first_release_platforms": sorted(REQUIRED_PLATFORMS),
            "unsupported_platforms": ["darwin", "macos"],
            "unsupported_platform_diagnostic": "SB_DIAG_PLATFORM_UNSUPPORTED_FIRST_RELEASE",
            "windows_release_scope": "x64_only_no_win32",
            "first_release_binary_scope": "engine_only",
            "cluster_production_execution": "external_provider_only",
            "cluster_stub_scope": "compile_link_only_non_mutating",
            "release_proof_is_evidence_only": True,
        },
        "matrix": matrix,
        "build_requirements": build_requirements,
        "release_layouts": release_layouts,
        "negative_platform_proofs": negative_platform_proofs,
        "host_result": host,
        "nonhost_platform_results": nonhost_records,
    }
    encoded = json.dumps(evidence, sort_keys=True, separators=(",", ":"))
    evidence["evidence_sha256"] = sha256_text(encoded)
    return evidence


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--configured-system-name", required=True)
    parser.add_argument("--configured-system-processor", required=True)
    parser.add_argument("--configured-cxx-compiler-id", required=True)
    parser.add_argument("--configured-cxx-compiler-version", required=True)
    parser.add_argument("--configured-cmake-version", required=True)
    parser.add_argument("--configured-cxx-standard", required=True)
    parser.add_argument("--cluster-provider-enabled", required=True)
    parser.add_argument("--cluster-provider-stub", required=True)
    parser.add_argument("--cluster-provider-external-library", default="")
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    project_root = args.project_root.resolve()
    build_root = args.build_root.resolve()
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

    evidence = build_evidence(repo_root, project_root, args)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"public_platform_matrix_output={output_record}")
    print(f"public_platform_matrix_sha256={evidence['evidence_sha256']}")
    print(f"public_platform_matrix_host_status={evidence['host_result']['status']}")
    print("public_platform_matrix_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
