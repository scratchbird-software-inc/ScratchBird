#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Standalone gate for release provenance, SBOM, license, and artifact evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import subprocess
import sys
from typing import Any


def fail(message: str) -> None:
    print(f"release_provenance_sbom_license_gate=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run_generator(repo_root: pathlib.Path, build_root: pathlib.Path, generator: pathlib.Path) -> pathlib.Path:
    output = build_root / "release_provenance" / "scratchbird_core_beta_release_evidence.json"
    result = subprocess.run(
        [
            sys.executable,
            str(generator),
            "--repo-root",
            str(repo_root),
            "--build-root",
            str(build_root),
            "--output",
            str(output),
        ],
        cwd=str(repo_root),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=120,
        check=False,
    )
    if result.returncode != 0:
        print(result.stdout)
        fail(f"generator_failed:exit={result.returncode}")
    if not output.exists():
        fail("generator_missing_output")
    return output


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def resolve_recorded_path(repo_root: pathlib.Path, build_root: pathlib.Path, path_text: str) -> pathlib.Path:
    if path_text.startswith("build/"):
        build_relative = pathlib.Path(path_text.removeprefix("build/"))
        if build_relative.parts and build_relative.parts[0] == build_root.name:
            build_relative = pathlib.Path(*build_relative.parts[1:])
        return build_root / build_relative
    path = pathlib.Path(path_text)
    if path.is_absolute():
        return path
    return repo_root / path


def validate_checksums(repo_root: pathlib.Path, build_root: pathlib.Path, checksums: list[dict[str, Any]]) -> None:
    present = [entry for entry in checksums if entry.get("present") is True]
    require(len(present) >= 6, "too_few_present_checksums")
    require(any(entry.get("location") == "build_tree" for entry in present), "missing_build_tree_checksum")
    require(any(entry.get("location") == "source_tree" for entry in present), "missing_source_tree_checksum")
    required_ids = {
        "configured_build_cache",
        "driver_package_manifest",
        "public_api_abi_manifest",
    }
    present_ids = {str(entry.get("artifact_id")) for entry in present}
    require(required_ids.issubset(present_ids), "missing_required_checksum_ids")
    for entry in present:
        checksum = str(entry.get("sha256", ""))
        require(len(checksum) == 64 and all(ch in "0123456789abcdef" for ch in checksum), "bad_checksum_shape")
        recorded_path = resolve_recorded_path(repo_root, build_root, str(entry.get("path", "")))
        require(recorded_path.exists(), f"checksum_path_missing:{entry.get('path')}")
        require(sha256_file(recorded_path) == checksum, f"checksum_mismatch:{entry.get('artifact_id')}")


def validate_sbom(components: list[dict[str, Any]]) -> None:
    ids = {str(component.get("component_id")) for component in components}
    for required in (
        "core_engine",
        "single_node_server",
        "sbsql_parser_worker",
        "sbsql_parser_udr",
        "sblr_runtime",
        "embedded_public_headers",
        "package_manifest",
        "cluster_provider_boundary",
        "driver:python",
        "driver:cpp",
        "tool:cli",
    ):
        require(required in ids, f"missing_sbom_component:{required}")
    require(any(str(component.get("component_id", "")).startswith("adaptor:") for component in components), "missing_adaptor_component")
    cluster = next(component for component in components if component.get("component_id") == "cluster_provider_boundary")
    require(cluster.get("scope") == "cluster_optional", "cluster_boundary_scope_overclaim")
    require("boundary" in str(cluster.get("support", "")), "cluster_boundary_support_missing")


def package_license(repo_root: pathlib.Path, relative_path: str) -> str:
    package_path = repo_root / relative_path
    require(package_path.exists(), f"license_package_missing:{relative_path}")
    try:
        data = json.loads(package_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        fail(f"license_package_json_invalid:{relative_path}:{exc.msg}")
    license_text = str(data.get("license", "")).strip()
    require(license_text not in {"", "unknown_not_declared"}, f"license_package_not_declared:{relative_path}")
    return license_text


def validate_licenses(repo_root: pathlib.Path, licenses: list[dict[str, Any]]) -> None:
    ids = {str(entry.get("component_id")) for entry in licenses}
    for required in (
        "scratchbird_private_core",
        "driver:node",
        "driver:odbc",
        "driver:r",
        "driver_manifest_rows_without_license_file",
    ):
        require(required in ids, f"missing_license_entry:{required}")
    node = next(entry for entry in licenses if entry.get("component_id") == "driver:node")
    expected_node_license = package_license(repo_root, "project/drivers/driver/node/package.json")
    require(
        node.get("license") == expected_node_license,
        f"node_driver_license_mismatch:{node.get('license')}:{expected_node_license}",
    )
    require(
        any(entry.get("classification") == "unknown_not_packaged" for entry in licenses),
        "missing_unknown_not_packaged_classification",
    )
    require(
        any(entry.get("classification") == "third_party_dev_dependency_not_packaged" for entry in licenses),
        "missing_third_party_dev_dependency_classification",
    )


def validate_generated_inventory(inventory: list[dict[str, Any]]) -> None:
    ids = {str(entry.get("artifact_id")) for entry in inventory}
    for required in (
        "parser_generated_registry_cpp",
        "parser_generated_registry_manifest",
        "function_completion_manifest_cpp",
        "sbsql_generated_fixture_tree",
        "configured_cmake_cache",
    ):
        require(required in ids, f"missing_generated_artifact:{required}")
    fixture_tree = next(entry for entry in inventory if entry.get("artifact_id") == "sbsql_generated_fixture_tree")
    require(int(fixture_tree.get("file_count", 0)) > 50, "generated_fixture_tree_too_small")


def validate_no_execution_plan_dependency(repo_root: pathlib.Path, paths: list[pathlib.Path]) -> None:
    for path in paths:
        text = path.read_text(encoding="utf-8")
        require("docs" "/execution-plans" not in text, f"execution_plan_dependency:{path.relative_to(repo_root)}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--build-root", required=True)
    parser.add_argument("--generator", required=True)
    args = parser.parse_args()

    repo_root = pathlib.Path(args.repo_root).resolve()
    build_root = pathlib.Path(args.build_root).resolve()
    generator = pathlib.Path(args.generator).resolve()
    validate_no_execution_plan_dependency(repo_root, [generator])

    output = run_generator(repo_root, build_root, generator)
    evidence = json.loads(output.read_text(encoding="utf-8"))
    require(evidence.get("schema_version") == 1, "bad_schema_version")
    require(evidence.get("policy", {}).get("execution_plan_independent") is True, "policy_not_execution_plan_independent")
    require("no positive closed-source cluster" in evidence.get("policy", {}).get("cluster_claim", ""), "cluster_claim_overbroad")
    require("SBLR/internal APIs" in evidence.get("policy", {}).get("engine_execution_boundary", ""), "engine_boundary_missing")

    build_metadata = evidence.get("build_metadata", {})
    require(build_metadata.get("cmake_cache", {}).get("present") is True, "missing_cmake_cache_metadata")
    require("cluster_provider_mode" in build_metadata, "missing_cluster_provider_mode")
    require("configured_options" in build_metadata, "missing_configured_options")

    source = evidence.get("source_provenance", {})
    require(source.get("git_available") is True, "git_metadata_unavailable")
    require(isinstance(source.get("dirty"), bool), "dirty_state_not_boolean")
    require(isinstance(source.get("dirty_entry_count"), int), "dirty_count_not_integer")

    validate_checksums(repo_root, build_root, evidence.get("checksums", []))
    validate_sbom(evidence.get("sbom_components", []))
    validate_licenses(repo_root, evidence.get("license_inventory", []))
    validate_generated_inventory(evidence.get("generated_artifact_inventory", []))
    print(f"release_provenance_output={output}")
    print(f"release_provenance_dirty={source.get('dirty')} dirty_entry_count={source.get('dirty_entry_count')}")
    print("release_provenance_sbom_license_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
