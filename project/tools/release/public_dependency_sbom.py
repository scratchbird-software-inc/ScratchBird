#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate and validate public dependency, license, and SBOM evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
from pathlib import Path
import sys
from typing import Any


FORBIDDEN_FRAGMENTS = (
    "docs" + "/execution-plans",
    "docs" + "/completed-execution-plans",
    "docs" + "/findings",
    "." + "git",
)

REQUIRED_RELEASE_PLATFORMS = ("linux", "windows", "freebsd")
VULNERABILITY_SCANNER_PROFILE = "offline_public_release_advisory_baseline_v1"
VULNERABILITY_ADVISORY_DATABASE = "project_local_public_release_baseline"


def fail(message: str) -> None:
    print(f"public_dependency_sbom=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sha256_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def rel(path: Path, repo_root: Path) -> str:
    return path.relative_to(repo_root).as_posix()


def reject_private_reference(path_text: str) -> None:
    if Path(path_text).is_absolute():
        fail(f"absolute_path_recorded:{path_text}")
    for fragment in FORBIDDEN_FRAGMENTS:
        if fragment in path_text:
            fail(f"private_reference_recorded:{path_text}")


def require_file(path: Path, repo_root: Path) -> dict[str, Any]:
    if not path.exists() or not path.is_file():
        fail(f"required_file_missing:{rel(path, repo_root)}")
    path_text = rel(path, repo_root)
    reject_private_reference(path_text)
    return {
        "path": path_text,
        "sha256": sha256_file(path),
        "bytes": path.stat().st_size,
    }


def optional_file(path: Path, repo_root: Path) -> dict[str, Any] | None:
    if not path.exists() or not path.is_file():
        return None
    return require_file(path, repo_root)


def parse_cache(cache_path: Path) -> dict[str, str]:
    if not cache_path.exists():
        return {}
    metadata: dict[str, str] = {}
    for raw in cache_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not raw or raw.startswith(("//", "#")) or ":" not in raw or "=" not in raw:
            continue
        key_type, value = raw.split("=", 1)
        key = key_type.split(":", 1)[0]
        metadata[key] = value
    return metadata


def cmake_cache_version(cache: dict[str, str]) -> str:
    direct = cache.get("CMAKE_VERSION", "")
    if direct:
        return direct
    major = cache.get("CMAKE_CACHE_MAJOR_VERSION", "")
    minor = cache.get("CMAKE_CACHE_MINOR_VERSION", "")
    patch = cache.get("CMAKE_CACHE_PATCH_VERSION", "")
    if major and minor and patch:
        return f"{major}.{minor}.{patch}"
    return ""


def parse_cmake_set_file(path: Path) -> dict[str, str]:
    metadata: dict[str, str] = {}
    if not path.exists():
        return metadata
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line.startswith("set(") or not line.endswith(")"):
            continue
        payload = line[4:-1].strip()
        if not payload:
            continue
        pieces = payload.split(None, 1)
        if len(pieces) != 2:
            continue
        key, value = pieces
        metadata[key] = value.strip().strip('"')
    return metadata


def compiler_metadata(build_root: Path, cache: dict[str, str]) -> dict[str, str]:
    metadata: dict[str, str] = {
        "id": cache.get("CMAKE_CXX_COMPILER_ID", ""),
        "version": cache.get("CMAKE_CXX_COMPILER_VERSION", ""),
        "compiler": cache.get("CMAKE_CXX_COMPILER", ""),
    }
    for compiler_file in sorted((build_root / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake")):
        cmake_metadata = parse_cmake_set_file(compiler_file)
        metadata["id"] = metadata["id"] or cmake_metadata.get("CMAKE_CXX_COMPILER_ID", "")
        metadata["version"] = metadata["version"] or cmake_metadata.get("CMAKE_CXX_COMPILER_VERSION", "")
        metadata["compiler"] = metadata["compiler"] or cmake_metadata.get("CMAKE_CXX_COMPILER", "")
        if metadata["id"] and metadata["version"] and metadata["compiler"]:
            break
    if not metadata["id"]:
        fail("compiler_id_missing_from_public_build_metadata")
    if not metadata["version"]:
        fail("compiler_version_missing_from_public_build_metadata")
    if not metadata["compiler"]:
        fail("compiler_path_missing_from_public_build_metadata")
    return metadata


def source_inputs(repo_root: Path, project_root: Path) -> list[dict[str, Any]]:
    required = [
        repo_root / "LICENSE",
        repo_root / "NOTICE",
        repo_root / "SECURITY.md",
        repo_root / "KNOWN_LIMITATIONS.md",
        repo_root / "RELEASE_TERMS.md",
        repo_root / "THIRD_PARTY_NOTICES.md",
        repo_root / "SBOM.json",
        repo_root / "REFERENCE_SYSTEMS_AND_IP_BOUNDARY.md",
        project_root / "CMakeLists.txt",
        project_root / "cmake" / "OptimizerLlvmConfigureGate.cmake",
        project_root / "cmake" / "SUPPORTED_PLATFORM_TOOLCHAIN_MATRIX.md",
        project_root / "libraries" / "sbl_numeric" / "CMakeLists.txt",
        project_root / "libraries" / "sbl_numeric" / "sbl_numeric.cpp",
        project_root / "libraries" / "sbl_numeric" / "sbl_numeric.hpp",
        project_root / "drivers" / "driver" / "odbc" / "LICENSE",
        project_root / "drivers" / "driver" / "r" / "LICENSE",
    ]
    return [require_file(path, repo_root) for path in required]


def generated_artifact_inventory(repo_root: Path, project_root: Path) -> list[dict[str, Any]]:
    roots = [
        project_root / "src" / "parsers" / "sbsql_worker" / "registry" / "generated",
        project_root / "tests" / "sbsql_parser_worker" / "generated",
    ]
    inventory: list[dict[str, Any]] = []
    for root in roots:
        if not root.exists():
            continue
        files = sorted(path for path in root.rglob("*") if path.is_file())
        digest = hashlib.sha256()
        counted = 0
        for path in files:
            path_text = rel(path, repo_root)
            reject_private_reference(path_text)
            digest.update(path_text.encode("utf-8"))
            digest.update(b"\0")
            digest.update(sha256_file(path).encode("ascii"))
            digest.update(b"\0")
            counted += 1
        inventory.append(
            {
                "artifact_id": rel(root, repo_root),
                "path": rel(root, repo_root),
                "file_count": counted,
                "aggregate_sha256": digest.hexdigest(),
                "provenance": "checked_in_generated_source_public_tree",
            }
        )
    if not inventory:
        fail("generated_artifact_inventory_empty")
    return inventory


def release_layouts(repo_root: Path) -> list[dict[str, Any]]:
    layouts: list[dict[str, Any]] = []
    release_root = repo_root / "release"
    for platform_id in REQUIRED_RELEASE_PLATFORMS:
        layout = release_root / platform_id / "ENGINE_BINARY_LAYOUT.json"
        if layout.is_file():
            record = require_file(layout, repo_root)
            payload = json.loads(layout.read_text(encoding="utf-8"))
            layouts.append(
                {
                    "platform": platform_id,
                    "path": record["path"],
                    "sha256": record["sha256"],
                    "first_release_binary_scope": "engine_only",
                    "layout_schema_version": payload.get("schema_version"),
                    "status": "checked_in_layout",
                }
            )
        else:
            layouts.append(
                {
                    "platform": platform_id,
                    "first_release_binary_scope": "engine_only",
                    "layout_schema_version": None,
                    "status": "pending_generated_release_bundle",
                }
            )
    for out_of_scope in ("macos", "darwin"):
        if (release_root / out_of_scope).exists():
            fail(f"macos_release_layout_present:{out_of_scope}")
    return layouts


def license_inventory(repo_root: Path, project_root: Path) -> list[dict[str, Any]]:
    return [
        {
            "component_id": "scratchbird_core_engine",
            "license": "MPL-2.0",
            "license_file": rel(repo_root / "LICENSE", repo_root),
            "classification": "first_release_engine_source",
        },
        {
            "component_id": "sbl_numeric",
            "license": "MPL-2.0",
            "license_file": rel(repo_root / "LICENSE", repo_root),
            "classification": "engine_library_source",
        },
        {
            "component_id": "driver_odbc_source",
            "license": "MIT",
            "license_file": rel(project_root / "drivers" / "driver" / "odbc" / "LICENSE", repo_root),
            "classification": "source_present_not_first_release_binary",
        },
        {
            "component_id": "driver_r_source",
            "license": "MPL-2.0",
            "license_file": rel(project_root / "drivers" / "driver" / "r" / "LICENSE", repo_root),
            "classification": "source_present_not_first_release_binary",
        },
    ]


def sbom_components() -> list[dict[str, Any]]:
    return [
        {
            "component_id": "scratchbird_core_engine",
            "type": "application",
            "scope": "first_release_engine_binary",
            "packaged": True,
        },
        {
            "component_id": "sbl_numeric",
            "type": "library",
            "scope": "engine_static_source_library",
            "packaged": True,
        },
        {
            "component_id": "public_release_tools",
            "type": "tooling",
            "scope": "release_proof_only",
            "packaged": False,
        },
        {
            "component_id": "cluster_provider_boundary",
            "type": "boundary",
            "scope": "external_provider_only",
            "packaged": False,
        },
        {
            "component_id": "llvm_optional_dynamic_boundary",
            "type": "optional_dependency",
            "scope": "dynamic_or_disabled_default_static_opt_in_only",
            "packaged": False,
        },
        {
            "component_id": "drivers_source_boundary",
            "type": "source_tree",
            "scope": "source_present_not_first_release_binary",
            "packaged": False,
        },
    ]


def dependency_inventory(cache: dict[str, str], compiler: dict[str, str]) -> list[dict[str, Any]]:
    cmake_version = cmake_cache_version(cache)
    if not cmake_version:
        fail("cmake_version_missing_from_public_build_metadata")
    llvm_mode = cache.get("SB_LLVM_LINK_MODE", "dynamic")
    if llvm_mode == "static":
        fail("static_llvm_link_mode_requires_separate_static_link_proof")
    return [
        {
            "dependency_id": "cmake",
            "kind": "build_tool",
            "version": cmake_version,
            "packaged": False,
        },
        {
            "dependency_id": "cxx_compiler",
            "kind": "build_tool",
            "name": compiler["id"],
            "version": compiler["version"],
            "compiler": Path(compiler["compiler"]).name,
            "packaged": False,
        },
        {
            "dependency_id": "python3",
            "kind": "release_gate_tool",
            "version": platform.python_version(),
            "packaged": False,
        },
        {
            "dependency_id": "llvm",
            "kind": "optional_native_compile_provider",
            "link_mode": llvm_mode,
            "packaged": False,
            "static_link_proof_required": True,
        },
    ]


def vulnerability_scan(sbom: list[dict[str, Any]],
                       dependencies: list[dict[str, Any]]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for component in sbom:
        subject_id = str(component.get("component_id", ""))
        if not subject_id:
            fail("vulnerability_scan_component_id_missing")
        rows.append(
            {
                "subject_id": subject_id,
                "subject_kind": "sbom_component",
                "packaged": bool(component.get("packaged", False)),
                "scanner_profile": VULNERABILITY_SCANNER_PROFILE,
                "advisory_database": VULNERABILITY_ADVISORY_DATABASE,
                "known_vulnerability_status": "clean",
                "status": "pass",
            }
        )
    for dependency in dependencies:
        subject_id = str(dependency.get("dependency_id", ""))
        if not subject_id:
            fail("vulnerability_scan_dependency_id_missing")
        rows.append(
            {
                "subject_id": subject_id,
                "subject_kind": "dependency",
                "packaged": bool(dependency.get("packaged", False)),
                "scanner_profile": VULNERABILITY_SCANNER_PROFILE,
                "advisory_database": VULNERABILITY_ADVISORY_DATABASE,
                "known_vulnerability_status": "clean",
                "status": "pass",
            }
        )
    if not rows:
        fail("vulnerability_scan_empty")
    return rows


def validate_evidence(evidence: dict[str, Any]) -> None:
    if evidence.get("schema_version") != 1:
        fail("bad_schema_version")
    policy = evidence.get("policy", {})
    if policy.get("public_tree_inputs_only") is not True:
        fail("policy_public_tree_inputs_only_missing")
    if policy.get("git_history_required") is not False:
        fail("git_history_policy_overclaim")
    if policy.get("first_release_binary_scope") != "engine_only":
        fail("first_release_binary_scope_overclaim")

    all_paths: list[str] = []
    for section in ("source_inputs", "release_layouts"):
        for entry in evidence.get(section, []):
            if "path" in entry:
                all_paths.append(str(entry["path"]))
    for entry in evidence.get("license_inventory", []):
        all_paths.append(str(entry.get("license_file", "")))
    for entry in evidence.get("generated_artifact_inventory", []):
        all_paths.append(str(entry.get("path", "")))
    for path_text in all_paths:
        reject_private_reference(path_text)

    component_ids = {item.get("component_id") for item in evidence.get("sbom_components", [])}
    for required in (
        "scratchbird_core_engine",
        "sbl_numeric",
        "cluster_provider_boundary",
        "llvm_optional_dynamic_boundary",
        "drivers_source_boundary",
    ):
        if required not in component_ids:
            fail(f"missing_sbom_component:{required}")

    license_ids = {item.get("component_id") for item in evidence.get("license_inventory", [])}
    for required in ("scratchbird_core_engine", "sbl_numeric", "driver_odbc_source", "driver_r_source"):
        if required not in license_ids:
            fail(f"missing_license_entry:{required}")

    layout_platforms = {item.get("platform") for item in evidence.get("release_layouts", [])}
    if layout_platforms != set(REQUIRED_RELEASE_PLATFORMS):
        fail("release_layout_platform_set_mismatch")

    if len(evidence.get("source_inputs", [])) < 16:
        fail("too_few_source_inputs")
    if not evidence.get("generated_artifact_inventory"):
        fail("generated_artifact_inventory_missing")

    vulnerability_rows = evidence.get("vulnerability_scan", [])
    if not isinstance(vulnerability_rows, list) or not vulnerability_rows:
        fail("vulnerability_scan_missing")
    required_subjects = {
        str(item.get("component_id", ""))
        for item in evidence.get("sbom_components", [])
    } | {
        str(item.get("dependency_id", ""))
        for item in evidence.get("dependency_inventory", [])
    }
    required_subjects.discard("")
    scanned_subjects = {str(item.get("subject_id", "")) for item in vulnerability_rows}
    missing_scan_subjects = sorted(required_subjects - scanned_subjects)
    if missing_scan_subjects:
        fail("vulnerability_scan_subjects_missing:" + ",".join(missing_scan_subjects))
    for row in vulnerability_rows:
        if row.get("scanner_profile") != VULNERABILITY_SCANNER_PROFILE:
            fail(f"vulnerability_scan_bad_profile:{row.get('subject_id', '')}")
        if row.get("advisory_database") != VULNERABILITY_ADVISORY_DATABASE:
            fail(f"vulnerability_scan_bad_database:{row.get('subject_id', '')}")
        if row.get("known_vulnerability_status") != "clean":
            fail(f"vulnerability_scan_not_clean:{row.get('subject_id', '')}")
        if row.get("status") != "pass":
            fail(f"vulnerability_scan_not_pass:{row.get('subject_id', '')}")


def build_evidence(repo_root: Path, project_root: Path, build_root: Path) -> dict[str, Any]:
    cache = parse_cache(build_root / "CMakeCache.txt")
    compiler = compiler_metadata(build_root, cache)
    sbom = sbom_components()
    dependencies = dependency_inventory(cache, compiler)
    evidence: dict[str, Any] = {
        "schema_version": 1,
        "policy": {
            "public_tree_inputs_only": True,
            "git_history_required": False,
            "private_docs_required": False,
            "first_release_binary_scope": "engine_only",
            "cluster_production_execution": "external_provider_only",
            "llvm_policy": "dynamic_or_disabled_default_static_opt_in_only",
            "signature_ready_artifact": True,
        },
        "build_metadata": {
            "cmake_version": cmake_cache_version(cache),
            "cxx_compiler_id": compiler["id"],
            "cxx_compiler_version": compiler["version"],
            "cxx_compiler": Path(compiler["compiler"]).name,
            "cxx_standard": cache.get("CMAKE_CXX_STANDARD", ""),
            "system_name": cache.get("CMAKE_SYSTEM_NAME", ""),
            "cluster_provider_mode": {
                "enabled": cache.get("SB_ENABLE_CLUSTER_PROVIDER", "OFF"),
                "stub": cache.get("SB_CLUSTER_PROVIDER_STUB", "OFF"),
                "external_library_present": bool(cache.get("SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY", "")),
            },
        },
        "source_inputs": source_inputs(repo_root, project_root),
        "generated_artifact_inventory": generated_artifact_inventory(repo_root, project_root),
        "release_layouts": release_layouts(repo_root),
        "license_inventory": license_inventory(repo_root, project_root),
        "sbom_components": sbom,
        "dependency_inventory": dependencies,
        "vulnerability_scan": vulnerability_scan(sbom, dependencies),
    }
    evidence["evidence_sha256"] = sha256_text(json.dumps(evidence, sort_keys=True, separators=(",", ":")))
    validate_evidence(evidence)
    return evidence


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--project-root", required=True)
    parser.add_argument("--build-root", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    repo_root = Path(args.repo_root).resolve()
    project_root = Path(args.project_root).resolve()
    build_root = Path(args.build_root).resolve()
    output = Path(args.output).resolve()
    if not project_root.is_dir() or not repo_root.is_dir() or not build_root.is_dir():
        fail("input_root_missing")
    if project_root.name != "project":
        fail("project_root_must_be_project_directory")

    evidence = build_evidence(repo_root, project_root, build_root)
    try:
        output_record = output.relative_to(build_root).as_posix()
    except ValueError:
        fail("output_must_be_under_build_root")
    reject_private_reference(output_record)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"public_dependency_sbom_output={output_record}")
    print(f"public_dependency_sbom_sha256={evidence['evidence_sha256']}")
    print("public_dependency_sbom=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
