#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate ScratchBird core beta release provenance/SBOM evidence."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import pathlib
import platform
import subprocess
from datetime import datetime, timezone
from typing import Any


def rel(path: pathlib.Path, root: pathlib.Path) -> str:
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return path.resolve().as_posix()


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run_git(repo_root: pathlib.Path, args: list[str]) -> tuple[bool, str]:
    try:
        result = subprocess.run(
            ["git", *args],
            cwd=str(repo_root),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=30,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        return False, str(exc)
    if result.returncode != 0:
        return False, (result.stderr or result.stdout).strip()
    return True, result.stdout.strip()


def parse_cmake_cache(build_root: pathlib.Path) -> dict[str, str]:
    cache_path = build_root / "CMakeCache.txt"
    values: dict[str, str] = {}
    if not cache_path.exists():
        return values
    for raw_line in cache_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not raw_line or raw_line.startswith(("//", "#")) or "=" not in raw_line:
            continue
        key_type, value = raw_line.split("=", 1)
        key = key_type.split(":", 1)[0]
        values[key] = value
    return values


def build_metadata(build_root: pathlib.Path) -> dict[str, Any]:
    cache = parse_cmake_cache(build_root)
    option_prefixes = ("SB_", "SCRATCHBIRD_")
    options = {
        key: value
        for key, value in sorted(cache.items())
        if key.startswith(option_prefixes)
        or key
        in {
            "CMAKE_BUILD_TYPE",
            "CMAKE_CXX_STANDARD",
            "CMAKE_C_COMPILER",
            "CMAKE_CXX_COMPILER",
            "CMAKE_SYSTEM_NAME",
            "CMAKE_SYSTEM_PROCESSOR",
        }
    }
    return {
        "cmake_cache": {
            "path": "build/CMakeCache.txt",
            "present": (build_root / "CMakeCache.txt").exists(),
        },
        "cmake": {
            "build_type": cache.get("CMAKE_BUILD_TYPE", "unknown"),
            "cxx_standard": cache.get("CMAKE_CXX_STANDARD", "unknown"),
            "c_compiler": cache.get("CMAKE_C_COMPILER", "unknown"),
            "cxx_compiler": cache.get("CMAKE_CXX_COMPILER", "unknown"),
        },
        "host": {
            "system": platform.system(),
            "release": platform.release(),
            "machine": platform.machine(),
            "python": platform.python_version(),
        },
        "configured_options": options,
        "cluster_provider_mode": {
            "SB_NONCLUSTER_ENGINE_PROFILE": cache.get("SB_NONCLUSTER_ENGINE_PROFILE", "unknown"),
            "SB_ENABLE_CLUSTER_PROVIDER": cache.get("SB_ENABLE_CLUSTER_PROVIDER", "unknown"),
            "SB_CLUSTER_PROVIDER_STUB": cache.get("SB_CLUSTER_PROVIDER_STUB", "unknown"),
            "SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY": cache.get(
                "SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY", "unknown"
            ),
            "claim": "records build mode only; core provenance does not claim closed-source cluster functionality",
        },
    }


def source_provenance(repo_root: pathlib.Path) -> dict[str, Any]:
    inside_ok, inside = run_git(repo_root, ["rev-parse", "--is-inside-work-tree"])
    if not inside_ok or inside != "true":
        return {
            "git_available": False,
            "diagnostic": inside or "git metadata unavailable",
            "dirty": "unknown",
        }
    commit_ok, commit = run_git(repo_root, ["rev-parse", "HEAD"])
    branch_ok, branch = run_git(repo_root, ["branch", "--show-current"])
    status_ok, status = run_git(repo_root, ["status", "--porcelain"])
    status_lines = status.splitlines() if status_ok and status else []
    return {
        "git_available": True,
        "commit": commit if commit_ok else "unavailable",
        "branch": branch if branch_ok and branch else "detached_or_unavailable",
        "dirty": bool(status_lines),
        "dirty_entry_count": len(status_lines),
        "dirty_entry_sample": status_lines[:25],
        "diagnostic": None if commit_ok and status_ok else "git metadata partially unavailable",
    }


def checksum_entries(repo_root: pathlib.Path, build_root: pathlib.Path) -> list[dict[str, Any]]:
    cache = parse_cmake_cache(build_root)
    target_platform = cache.get("SB_PUBLIC_TARGET_PLATFORM", "linux")
    public_output = build_root / "output" / target_platform
    candidates = [
        ("build_tree", build_root / "CMakeCache.txt", "configured_build_cache"),
        ("build_tree", public_output / "lib/libSBcore_static.a", "core_engine_static_library"),
        ("build_tree", public_output / "lib/libSBcore.so", "core_engine_shared_library"),
        ("build_tree", public_output / "bin/SBsrv", "single_node_server_binary"),
        ("build_tree", public_output / "bin/SBgate", "listener_binary"),
        ("build_tree", public_output / "bin/SBParser", "sbsql_parser_worker_binary"),
        (
            "build_tree",
            public_output / "lib/libSBParser_udr.a",
            "sbsql_parser_udr_static_library",
        ),
        (
            "source_tree",
            repo_root / "project/docs/public_api/CORE_BETA_PUBLIC_API_ABI_MANIFEST.json",
            "public_api_abi_manifest",
        ),
        ("source_tree", repo_root / "project/drivers/DriverPackageManifest.csv", "driver_package_manifest"),
        ("source_tree", repo_root / "project/examples/core_beta_qa/manifest.json", "qa_examples_manifest"),
        (
            "source_tree",
            repo_root / "project/tests/performance/BETA_PERFORMANCE_BASELINE_THRESHOLDS.json",
            "performance_threshold_manifest",
        ),
    ]
    entries: list[dict[str, Any]] = []
    for location, path, artifact_id in candidates:
        if not path.exists() or not path.is_file():
            entries.append(
                {
                    "artifact_id": artifact_id,
                    "location": location,
                    "path": rel(path, repo_root),
                    "present": False,
                    "diagnostic": "not_built_or_not_packaged",
                }
            )
            continue
        entries.append(
            {
                "artifact_id": artifact_id,
                "location": location,
                "path": rel(path, repo_root),
                "present": True,
                "size_bytes": path.stat().st_size,
                "sha256": sha256_file(path),
            }
        )
    return entries


def load_driver_manifest(repo_root: pathlib.Path) -> list[dict[str, str]]:
    path = repo_root / "project/drivers/DriverPackageManifest.csv"
    if not path.exists():
        return []
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def sbom_components(repo_root: pathlib.Path) -> list[dict[str, Any]]:
    static_components = [
        ("core_engine", "core_engine", "core", "project/src/engine", "SBLR/internal API execution"),
        ("single_node_server", "server", "core", "project/src/server", "single-node server route"),
        ("listener_ipc", "listener_ipc", "core", "project/src/listener", "listener and IPC admission"),
        ("sbsql_parser_worker", "parser", "core", "project/src/parsers/sbsql_worker", "SBsql to SBLR"),
        (
            "sbsql_parser_udr",
            "udr_package",
            "core",
            "project/src/udr/sbu_sbsql_parser_support",
            "trusted parser support UDR",
        ),
        ("sblr_runtime", "sblr", "core", "project/src/engine/sblr", "engine SBLR envelope and dispatch"),
        ("embedded_public_headers", "embedded_api", "core", "project/include/scratchbird", "public ABI headers"),
        (
            "cluster_provider_boundary",
            "cluster_boundary",
            "cluster_optional",
            "project/src/cluster_provider",
            "provider/stub/no-cluster boundary only",
        ),
        ("package_manifest", "package", "core", "project/drivers/DriverPackageManifest.csv", "driver package index"),
    ]
    components: list[dict[str, Any]] = []
    for component_id, category, scope, path_text, support in static_components:
        path = repo_root / path_text
        components.append(
            {
                "component_id": component_id,
                "category": category,
                "scope": scope,
                "path": path_text,
                "present": path.exists(),
                "support": support,
            }
        )
    for row in load_driver_manifest(repo_root):
        component_id = row.get("component_id", "")
        if not component_id:
            continue
        components.append(
            {
                "component_id": component_id,
                "category": row.get("category", "driver_manifest"),
                "scope": "core_client_surface",
                "path": row.get("source_path", ""),
                "present": (repo_root / row.get("source_path", "")).exists(),
                "support": row.get("driver_status", "unknown"),
                "conformance_profile_ref": row.get("conformance_profile_ref", ""),
            }
        )
    return components


def package_license(path: pathlib.Path) -> str:
    if not path.exists():
        return "not_present"
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return "invalid_package_json"
    return str(data.get("license", "unknown_not_declared"))


def license_inventory(repo_root: pathlib.Path) -> list[dict[str, Any]]:
    entries = [
        {
            "component_id": "scratchbird_private_core",
            "classification": "private_not_licensed_for_external_use",
            "license": "proprietary_private",
            "reference_path": "docs/legal/private-contract-and-trade-secret-notice.md",
            "present": (repo_root / "docs/legal/private-contract-and-trade-secret-notice.md").exists(),
        },
        {
            "component_id": "driver:node",
            "classification": "in_tree_driver_package",
            "license": package_license(repo_root / "project/drivers/driver/node/package.json"),
            "reference_path": "project/drivers/driver/node/package.json",
            "present": (repo_root / "project/drivers/driver/node/package.json").exists(),
        },
        {
            "component_id": "adaptor:scratchbird-prisma-adapter",
            "classification": "in_tree_adaptor_package",
            "license": package_license(
                repo_root / "project/drivers/adaptor/scratchbird-prisma-adapter/package.json"
            ),
            "reference_path": "project/drivers/adaptor/scratchbird-prisma-adapter/package.json",
            "present": (repo_root / "project/drivers/adaptor/scratchbird-prisma-adapter/package.json").exists(),
        },
        {
            "component_id": "adaptor:scratchbird-typeorm-adapter",
            "classification": "in_tree_adaptor_package",
            "license": package_license(
                repo_root / "project/drivers/adaptor/scratchbird-typeorm-adapter/package.json"
            ),
            "reference_path": "project/drivers/adaptor/scratchbird-typeorm-adapter/package.json",
            "present": (repo_root / "project/drivers/adaptor/scratchbird-typeorm-adapter/package.json").exists(),
        },
        {
            "component_id": "driver:odbc",
            "classification": "in_tree_driver_license_reference",
            "license": "see_file",
            "reference_path": "project/drivers/driver/odbc/LICENSE",
            "present": (repo_root / "project/drivers/driver/odbc/LICENSE").exists(),
        },
        {
            "component_id": "driver:r",
            "classification": "in_tree_driver_license_reference",
            "license": "see_file",
            "reference_path": "project/drivers/driver/r/LICENSE",
            "present": (repo_root / "project/drivers/driver/r/LICENSE").exists(),
        },
        {
            "component_id": "driver_manifest_rows_without_license_file",
            "classification": "unknown_not_packaged",
            "license": "unknown_not_packaged",
            "reference_path": "project/drivers/DriverPackageManifest.csv",
            "present": (repo_root / "project/drivers/DriverPackageManifest.csv").exists(),
            "diagnostic": "Driver package manifest tracks package surfaces; per-lane license files remain explicit where present.",
        },
    ]
    node_package = repo_root / "project/drivers/driver/node/package.json"
    if node_package.exists():
        try:
            data = json.loads(node_package.read_text(encoding="utf-8"))
            for name, version in sorted(data.get("devDependencies", {}).items()):
                entries.append(
                    {
                        "component_id": f"node_dev_dependency:{name}",
                        "classification": "third_party_dev_dependency_not_packaged",
                        "license": "unknown_not_packaged",
                        "reference_path": "project/drivers/driver/node/package.json",
                        "present": True,
                        "version": version,
                    }
                )
        except json.JSONDecodeError:
            pass
    return entries


def generated_artifact_inventory(repo_root: pathlib.Path, build_root: pathlib.Path) -> list[dict[str, Any]]:
    explicit = [
        (
            "parser_generated_registry_cpp",
            "source_controlled_generated_parser_registry",
            repo_root / "project/src/parsers/sbsql_worker/registry/generated/sbsql_generated_registry.cpp",
        ),
        (
            "parser_generated_registry_manifest",
            "source_controlled_generated_parser_registry",
            repo_root / "project/src/parsers/sbsql_worker/registry/generated/sbsql_generated_registry.manifest",
        ),
        (
            "function_completion_manifest_cpp",
            "source_controlled_generated_sblr_function_manifest",
            repo_root / "project/src/engine/functions/generated/function_completion_manifest.cpp",
        ),
        (
            "sbsql_generated_fixture_tree",
            "source_controlled_generated_test_fixtures",
            repo_root / "project/tests/sbsql_parser_worker/generated",
        ),
        ("configured_cmake_cache", "build_generated_configuration", build_root / "CMakeCache.txt"),
    ]
    entries: list[dict[str, Any]] = []
    for artifact_id, kind, path in explicit:
        entry: dict[str, Any] = {
            "artifact_id": artifact_id,
            "kind": kind,
            "path": rel(path, repo_root),
            "present": path.exists(),
        }
        if path.is_file():
            entry["sha256"] = sha256_file(path)
            entry["size_bytes"] = path.stat().st_size
        elif path.is_dir():
            files = [candidate for candidate in path.rglob("*") if candidate.is_file() and "__pycache__" not in candidate.parts]
            entry["file_count"] = len(files)
            entry["sample"] = [rel(candidate, repo_root) for candidate in sorted(files)[:15]]
        entries.append(entry)
    return entries


def generate(repo_root: pathlib.Path, build_root: pathlib.Path) -> dict[str, Any]:
    return {
        "schema_version": 1,
        "evidence_id": "scratchbird-core-beta-release-provenance-sbom-license",
        "generated_at_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
        "policy": {
            "execution_plan_independent": True,
            "cluster_claim": "provider/stub/no-cluster build mode only; no positive closed-source cluster functionality claimed",
            "engine_execution_boundary": "engine executes SBLR/internal APIs; SQL text is parser/client input only",
        },
        "build_metadata": build_metadata(build_root),
        "source_provenance": source_provenance(repo_root),
        "checksums": checksum_entries(repo_root, build_root),
        "sbom_components": sbom_components(repo_root),
        "license_inventory": license_inventory(repo_root),
        "generated_artifact_inventory": generated_artifact_inventory(repo_root, build_root),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--build-root", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    repo_root = pathlib.Path(args.repo_root).resolve()
    build_root = pathlib.Path(args.build_root).resolve()
    output = pathlib.Path(args.output).resolve()
    evidence = generate(repo_root, build_root)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
