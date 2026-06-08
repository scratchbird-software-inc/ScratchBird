#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate platform environment verification scripts and requirement coverage."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import shutil
import sys
from typing import Any


# PUBLIC_PLATFORM_ENV_VERIFY

PLATFORM_REQUIREMENTS: dict[str, dict[str, Any]] = {
    "linux": {
        "wrapper": "project/tools/release/platform_env/verify-linux.sh",
        "marker": "PUBLIC_PLATFORM_ENV_VERIFY_LINUX",
        "doc": "docs/build_requirements/linux/README.md",
        "tokens": (
            "Ubuntu 24.04 LTS x86_64",
            "gcc-13",
            "g++-13",
            "clang-18",
            "cmake",
            "ninja-build",
            "python3",
            "libssl-dev",
            "libicu-dev",
            "libxml2-dev",
            "zlib1g-dev",
            "liblz4-dev",
            "libzstd-dev",
            "libgeos-dev",
            "libproj-dev",
            "libgtest-dev",
            "unixodbc-dev",
            "LLVM 23+",
            "clang-tidy-18",
            "cppcheck",
            "engine_listener_enterprise",
        ),
    },
    "windows": {
        "wrapper": "project/tools/release/platform_env/verify-windows.ps1",
        "marker": "PUBLIC_PLATFORM_ENV_VERIFY_WINDOWS",
        "doc": "docs/build_requirements/windows/README.md",
        "tokens": (
            "Windows 11 x64",
            "Windows Server 2022/2025 x64",
            "MSYS2 UCRT64",
            "GNU C++",
            "gcc 15",
            "CMake 3.29",
            "Ninja 1.11",
            "Python 3.12",
            "mingw-w64-ucrt-x86_64-openssl",
            "mingw-w64-ucrt-x86_64-icu",
            "mingw-w64-ucrt-x86_64-libxml2",
            "mingw-w64-ucrt-x86_64-zlib",
            "mingw-w64-ucrt-x86_64-lz4",
            "mingw-w64-ucrt-x86_64-zstd",
            "mingw-w64-ucrt-x86_64-geos",
            "mingw-w64-ucrt-x86_64-proj",
            "mingw-w64-ucrt-x86_64-gtest",
            "odbc32",
            "LLVM 23+",
            "engine_listener_enterprise",
        ),
    },
    "freebsd": {
        "wrapper": "project/tools/release/platform_env/verify-freebsd.sh",
        "marker": "PUBLIC_PLATFORM_ENV_VERIFY_FREEBSD",
        "doc": "docs/build_requirements/freebsd/README.md",
        "tokens": (
            "FreeBSD 14.x x86_64",
            "cmake",
            "ninja",
            "python311",
            "llvm18",
            "openssl",
            "icu",
            "libxml2",
            "zlib",
            "lz4",
            "zstd",
            "geos",
            "proj",
            "googletest",
            "unixODBC",
            "cppcheck",
            "engine_listener_enterprise",
        ),
    },
}

COMMON_REQUIREMENT_TOKENS = (
    "Common Requirements",
    "CMake 3.25 minimum",
    "Ninja 1.11 or newer",
    "C++23 compiler",
    "Python 3.11 or newer",
    "OpenSSL 3.x development headers and libraries",
    "ICU development headers and libraries",
    "LibXML2 development headers and libraries",
    "zlib development headers and libraries",
    "LZ4 development headers and libraries",
    "Zstd development headers and libraries",
    "GEOS and PROJ",
    "GoogleTest",
    "ODBC SDK or driver manager",
    "LLVM 23 or newer",
)

NATIVE_PROOF_LABELS = (
    "public_release_correctness",
    "engine_listener_enterprise",
)

NATIVE_PROOF_CONTRACTS: dict[str, dict[str, Any]] = {
    "linux": {
        "build_root": "build-linux-public-release-proof",
        "shell": "sh",
        "configure_command": (
            "cmake -S project -B build-linux-public-release-proof -G Ninja "
            "-DCMAKE_BUILD_TYPE=Release -DSB_BUILD_TESTS=ON "
            "-DSB_BUILD_PUBLIC_RELEASE_CORRECTNESS=ON "
            "-DSB_NONCLUSTER_ENGINE_PROFILE=release-complete "
            "-DSB_ENABLE_CLUSTER_PROVIDER=OFF "
            "-DSCRATCHBIRD_ENABLE_DEBUG_LOGS=OFF "
            "-DSCRATCHBIRD_ENABLE_HOTPATH_TRACE=OFF "
            "-DSCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE=OFF "
            "-DSCRATCHBIRD_ENABLE_PREPARED_TRACE=OFF "
            "-DSB_LLVM_LINK_MODE=dynamic"
        ),
        "build_command": "cmake --build build-linux-public-release-proof -j2",
        "ctest_commands": (
            "ctest --test-dir build-linux-public-release-proof -L public_release_correctness --output-on-failure",
            "ctest --test-dir build-linux-public-release-proof -L engine_listener_enterprise --output-on-failure",
        ),
    },
    "windows": {
        "build_root": "build-windows-public-release-proof",
        "shell": "Developer PowerShell for VS 2022",
        "configure_command": (
            "cmake -S project -B build-windows-public-release-proof -G Ninja "
            "-DCMAKE_BUILD_TYPE=Release "
            "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT\\scripts\\buildsystems\\vcpkg.cmake "
            "-DSB_BUILD_TESTS=ON -DSB_BUILD_PUBLIC_RELEASE_CORRECTNESS=ON "
            "-DSB_NONCLUSTER_ENGINE_PROFILE=release-complete "
            "-DSB_ENABLE_CLUSTER_PROVIDER=OFF "
            "-DSCRATCHBIRD_ENABLE_DEBUG_LOGS=OFF "
            "-DSCRATCHBIRD_ENABLE_HOTPATH_TRACE=OFF "
            "-DSCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE=OFF "
            "-DSCRATCHBIRD_ENABLE_PREPARED_TRACE=OFF "
            "-DSB_LLVM_LINK_MODE=dynamic"
        ),
        "build_command": "cmake --build build-windows-public-release-proof -j 2",
        "ctest_commands": (
            "ctest --test-dir build-windows-public-release-proof -L public_release_correctness --output-on-failure",
            "ctest --test-dir build-windows-public-release-proof -L engine_listener_enterprise --output-on-failure",
        ),
    },
    "freebsd": {
        "build_root": "build-freebsd-public-release-proof",
        "shell": "sh",
        "configure_command": (
            "cmake -S project -B build-freebsd-public-release-proof -G Ninja "
            "-DCMAKE_BUILD_TYPE=Release -DSB_BUILD_TESTS=ON "
            "-DSB_BUILD_PUBLIC_RELEASE_CORRECTNESS=ON "
            "-DSB_NONCLUSTER_ENGINE_PROFILE=release-complete "
            "-DSB_ENABLE_CLUSTER_PROVIDER=OFF "
            "-DSCRATCHBIRD_ENABLE_DEBUG_LOGS=OFF "
            "-DSCRATCHBIRD_ENABLE_HOTPATH_TRACE=OFF "
            "-DSCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE=OFF "
            "-DSCRATCHBIRD_ENABLE_PREPARED_TRACE=OFF "
            "-DSB_LLVM_LINK_MODE=dynamic"
        ),
        "build_command": "cmake --build build-freebsd-public-release-proof -j2",
        "ctest_commands": (
            "ctest --test-dir build-freebsd-public-release-proof -L public_release_correctness --output-on-failure",
            "ctest --test-dir build-freebsd-public-release-proof -L engine_listener_enterprise --output-on-failure",
        ),
    },
}

EVIDENCE_CHECKS: tuple[dict[str, Any], ...] = (
    {
        "surface": "platform_matrix_gate",
        "path": "project/tools/release/public_platform_matrix_gate.py",
        "tokens": (
            "REQUIRED_PLATFORMS",
            "compiler_families",
            "matrix_compilers",
            "SB_LLVM_LINK_MODE=dynamic",
            "public_platform_matrix_gate=passed",
        ),
    },
    {
        "surface": "release_gate_wiring",
        "path": "project/tests/release/CMakeLists.txt",
        "tokens": (
            "PUBLIC_PLATFORM_ENV_VERIFY_GATE",
            "public_platform_env_verify_gate",
            "public_platform_environment_verify.py",
            "PCR-GATE-156",
        ),
    },
)


def fail(message: str) -> None:
    print(f"public_platform_env_verify_gate=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def read_text(repo_root: Path, relative_path: str) -> str:
    path = repo_root / relative_path
    require(path.is_file(), f"source_missing:{relative_path}")
    try:
        return path.read_text(encoding="utf-8")
    except UnicodeDecodeError as exc:
        fail(f"utf8_decode_failed:{relative_path}:{exc}")
    except OSError as exc:
        fail(f"read_failed:{relative_path}:{exc}")


def parse_cache(cache_path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    if not cache_path.is_file():
        return values
    for raw in cache_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not raw or raw.startswith(("//", "#")) or ":" not in raw or "=" not in raw:
            continue
        key_type, value = raw.split("=", 1)
        key = key_type.split(":", 1)[0]
        values[key] = value
    return values


def parse_cmake_set_file(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    if not path.is_file():
        return values
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line.startswith("set(") or not line.endswith(")"):
            continue
        payload = line[4:-1].strip()
        pieces = payload.split(None, 1)
        if len(pieces) != 2:
            continue
        values[pieces[0]] = pieces[1].strip().strip('"')
    return values


def compiler_metadata(build_root: Path, cache: dict[str, str]) -> dict[str, str]:
    metadata = {
        "id": cache.get("CMAKE_CXX_COMPILER_ID", ""),
        "version": cache.get("CMAKE_CXX_COMPILER_VERSION", ""),
        "compiler": cache.get("CMAKE_CXX_COMPILER", ""),
        "features": "",
    }
    for compiler_file in sorted((build_root / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake")):
        cmake_metadata = parse_cmake_set_file(compiler_file)
        metadata["id"] = metadata["id"] or cmake_metadata.get("CMAKE_CXX_COMPILER_ID", "")
        metadata["version"] = metadata["version"] or cmake_metadata.get("CMAKE_CXX_COMPILER_VERSION", "")
        metadata["compiler"] = metadata["compiler"] or cmake_metadata.get("CMAKE_CXX_COMPILER", "")
        metadata["features"] = metadata["features"] or cmake_metadata.get("CMAKE_CXX_COMPILE_FEATURES", "")
        if metadata["id"] and metadata["version"] and metadata["compiler"] and metadata["features"]:
            break
    require(bool(metadata["id"]), "compiler_id_missing")
    require(bool(metadata["version"]), "compiler_version_missing")
    require(bool(metadata["compiler"]), "compiler_path_missing")
    require("cxx_std_23" in metadata["features"], "compiler_cxx23_feature_missing")
    return metadata


def cmake_version(cache: dict[str, str]) -> str:
    direct = cache.get("CMAKE_VERSION", "")
    if direct:
        return direct
    major = cache.get("CMAKE_CACHE_MAJOR_VERSION", "")
    minor = cache.get("CMAKE_CACHE_MINOR_VERSION", "")
    patch = cache.get("CMAKE_CACHE_PATCH_VERSION", "")
    if major and minor and patch:
        return f"{major}.{minor}.{patch}"
    return ""


def validate_tokens(repo_root: Path, surface: str, relative_path: str, tokens: tuple[str, ...]) -> dict[str, Any]:
    text = read_text(repo_root, relative_path)
    token_digests: list[str] = []
    for token in tokens:
        require(token in text, f"token_missing:{surface}:{relative_path}:{token}")
        token_digests.append(sha256_text(token))
    return {
        "surface": surface,
        "path": relative_path,
        "token_count": len(tokens),
        "source_sha256": sha256_text(text),
        "token_digest_sha256": sha256_text("\n".join(token_digests) + "\n"),
        "status": "pass",
    }


def validate_wrapper(repo_root: Path, platform_id: str, spec: dict[str, Any]) -> dict[str, Any]:
    tokens = (
        spec["marker"],
        "public_platform_environment_verify.py",
        f"--platform {platform_id}",
    )
    return validate_tokens(repo_root, f"{platform_id}_wrapper", spec["wrapper"], tokens)


def validate_build_metadata(build_root: Path) -> dict[str, Any]:
    cache = parse_cache(build_root / "CMakeCache.txt")
    require(bool(cache), "cmake_cache_missing")
    compiler = compiler_metadata(build_root, cache)
    required_keys = (
        "CMAKE_BUILD_TYPE",
        "SB_BUILD_PUBLIC_RELEASE_CORRECTNESS",
    )
    for key in required_keys:
        require(bool(cache.get(key)), f"cmake_cache_key_missing:{key}")
    require(cache["CMAKE_BUILD_TYPE"] == "Release", "cmake_build_type_not_release")
    require(cache["SB_BUILD_PUBLIC_RELEASE_CORRECTNESS"] == "ON",
            "public_release_correctness_not_enabled")
    version = cmake_version(cache)
    require(bool(version), "cmake_version_missing")
    return {
        "surface": "configured_host_build_metadata",
        "path": "build/public_release_correctness/CMakeCache.txt",
        "token_count": len(required_keys),
        "source_sha256": sha256_text(
            "|".join(cache.get(key, "") for key in required_keys)
            + "|"
            + version
            + "|"
            + compiler["id"]
            + "|"
            + compiler["version"]
        ),
        "token_digest_sha256": sha256_text("\n".join(required_keys) + "\n"),
        "status": "pass",
        "cmake_version": version,
        "compiler_id": compiler["id"],
        "compiler_version": compiler["version"],
    }


def validate_host_tools() -> dict[str, Any]:
    tools = ("cmake", "python3")
    found = {tool: shutil.which(tool) is not None for tool in tools}
    for tool, present in found.items():
        require(present, f"host_tool_missing:{tool}")
    return {
        "surface": "configured_host_tool_probe",
        "path": "PATH",
        "token_count": len(tools),
        "source_sha256": sha256_text(json.dumps(found, sort_keys=True)),
        "token_digest_sha256": sha256_text("\n".join(tools) + "\n"),
        "status": "pass",
        "host_tools": found,
    }


def validate_native_proof_contract(
    repo_root: Path, platform_id: str, spec: dict[str, Any]
) -> tuple[dict[str, Any], dict[str, Any]]:
    contract = NATIVE_PROOF_CONTRACTS[platform_id]
    text = read_text(repo_root, spec["doc"])
    required_tokens = (
        "SB_BUILD_PUBLIC_RELEASE_CORRECTNESS=ON",
        "SB_NONCLUSTER_ENGINE_PROFILE=release-complete",
        "SB_ENABLE_CLUSTER_PROVIDER=OFF",
        "SB_LLVM_LINK_MODE=dynamic",
        *(f"-L {label} --output-on-failure" for label in NATIVE_PROOF_LABELS),
    )
    token_digests: list[str] = []
    for token in required_tokens:
        require(token in text, f"native_contract_token_missing:{platform_id}:{token}")
        token_digests.append(sha256_text(token))
    commands = [
        contract["configure_command"],
        contract["build_command"],
        *contract["ctest_commands"],
    ]
    normalized_contract = {
        "platform": platform_id,
        "doc": spec["doc"],
        "build_root": contract["build_root"],
        "shell": contract["shell"],
        "commands": commands,
        "required_ctest_labels": list(NATIVE_PROOF_LABELS),
        "native_runner_required": True,
        "linux_host_may_not_claim_nonhost_completion": True,
        "completion_status": "native_runner_required_until_platform_artifacts_pass",
        "command_sha256": sha256_text("\n".join(commands) + "\n"),
    }
    record = {
        "surface": f"{platform_id}_native_proof_contract",
        "path": spec["doc"],
        "token_count": len(required_tokens),
        "source_sha256": sha256_text(text),
        "token_digest_sha256": sha256_text("\n".join(token_digests) + "\n"),
        "status": "pass",
        "platform": platform_id,
        "native_contract_sha256": normalized_contract["command_sha256"],
        "required_ctest_labels": json.dumps(list(NATIVE_PROOF_LABELS), sort_keys=True),
        "native_completion_status": normalized_contract["completion_status"],
    }
    return record, normalized_contract


def write_csv(path: Path, records: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=[
                "surface",
                "path",
                "token_count",
                "source_sha256",
                "token_digest_sha256",
                "status",
                "cmake_version",
                "compiler_id",
                "compiler_version",
                "host_tools",
                "platform",
                "native_contract_sha256",
                "required_ctest_labels",
                "native_completion_status",
            ],
        )
        writer.writeheader()
        for record in records:
            writer.writerow(
                {
                    "surface": record["surface"],
                    "path": record["path"],
                    "token_count": record["token_count"],
                    "source_sha256": record["source_sha256"],
                    "token_digest_sha256": record["token_digest_sha256"],
                    "status": record["status"],
                    "cmake_version": record.get("cmake_version", ""),
                    "compiler_id": record.get("compiler_id", ""),
                    "compiler_version": record.get("compiler_version", ""),
                    "host_tools": json.dumps(record.get("host_tools", ""), sort_keys=True),
                    "platform": record.get("platform", ""),
                    "native_contract_sha256": record.get("native_contract_sha256", ""),
                    "required_ctest_labels": record.get("required_ctest_labels", ""),
                    "native_completion_status": record.get("native_completion_status", ""),
                }
            )


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--build-root", required=True, type=Path)
    parser.add_argument("--platform", choices=sorted(PLATFORM_REQUIREMENTS), default=None)
    parser.add_argument("--csv-output", required=True, type=Path)
    parser.add_argument("--evidence-output", required=True, type=Path)
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    repo_root = args.repo_root.resolve()
    platforms = [args.platform] if args.platform else sorted(PLATFORM_REQUIREMENTS)
    records: list[dict[str, Any]] = [
        validate_tokens(
            repo_root,
            "common_build_requirements",
            "docs/build_requirements/README.md",
            COMMON_REQUIREMENT_TOKENS,
        )
    ]
    for platform_id in platforms:
        spec = PLATFORM_REQUIREMENTS[platform_id]
        records.append(validate_tokens(repo_root, f"{platform_id}_requirements", spec["doc"], spec["tokens"]))
        records.append(validate_wrapper(repo_root, platform_id, spec))
    native_contracts: list[dict[str, Any]] = []
    for platform_id in platforms:
        record, contract = validate_native_proof_contract(
            repo_root, platform_id, PLATFORM_REQUIREMENTS[platform_id]
        )
        records.append(record)
        native_contracts.append(contract)
    records.extend(validate_tokens(repo_root, check["surface"], check["path"], check["tokens"])
                   for check in EVIDENCE_CHECKS)
    records.append(validate_build_metadata(args.build_root.resolve()))
    records.append(validate_host_tools())
    matrix_text = "\n".join(
        f"{record['surface']},{record['path']},{record['status']}" for record in records
    ) + "\n"
    payload = {
        "gate": "PUBLIC_PLATFORM_ENV_VERIFY_GATE",
        "marker": "PUBLIC_PLATFORM_ENV_VERIFY",
        "status": "pass",
        "checks": records,
        "check_count": len(records),
        "platforms": platforms,
        "native_runner_contracts": native_contracts,
        "matrix_sha256": sha256_text(matrix_text),
        "policy": {
            "per_platform_verifier_scripts_present": True,
            "required_toolchain_dependencies_declared": True,
            "current_build_metadata_checked": True,
            "unavailable_platforms_are_not_proven_by_host_gate": True,
            "native_platform_completion_requires_engine_listener_enterprise_label": True,
            "windows_release_scope": "x64_only_no_win32",
        },
    }
    write_csv(args.csv_output, records)
    write_json(args.evidence_output, payload)
    print(
        "public_platform_env_verify_gate=passed "
        f"checks={len(records)} matrix_sha256={payload['matrix_sha256']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
