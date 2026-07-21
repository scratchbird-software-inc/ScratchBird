#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Fail closed unless a staged distribution contains native SB components only."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

import stage_native_release_bundle as native


ROOT_METADATA = {
    "STANDALONE_OUTPUT_MANIFEST.json",
    "NATIVE_RELEASE_PROFILE.json",
    "PUBLIC_RELEASE_ARTIFACT_MANIFEST.json",
}
ROOT_DIRECTORIES = {"bin", "lib", "etc", "share"}


def fail(message: str) -> None:
    print(f"verify_native_release_bundle=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def file_names(root: Path) -> set[str]:
    if not root.is_dir():
        fail(f"required_directory_missing:{root}")
    names: set[str] = set()
    for path in root.iterdir():
        if path.is_symlink():
            fail(f"symlink_forbidden:{path}")
        if not path.is_file():
            fail(f"nested_binary_or_library_path_forbidden:{path}")
        names.add(path.name)
    return names


def directory_names(root: Path, *, allow_regular_files: bool = False) -> set[str]:
    """Return an exact directory inventory, rejecting files and links.

    The native release root is a closed server-only surface.  Checking only
    its known children lets an unrecognised directory carry a client binary
    while the normal bin/lib/share checks still pass.
    """

    if not root.is_dir():
        fail(f"required_directory_missing:{root}")
    names: set[str] = set()
    for path in root.iterdir():
        if path.is_symlink():
            fail(f"non_directory_layout_entry:{path}")
        if path.is_file() and allow_regular_files:
            continue
        if not path.is_dir():
            fail(f"non_directory_layout_entry:{path}")
        names.add(path.name)
    return names


def require_exact_directory_set(
    root: Path,
    expected: set[str],
    label: str,
    *,
    allow_regular_files: bool = False,
) -> None:
    actual = directory_names(root, allow_regular_files=allow_regular_files)
    if actual != expected:
        fail(
            f"{label}_directory_set_mismatch:"
            f"missing={sorted(expected - actual)}:"
            f"unexpected={sorted(actual - expected)}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifact_root", type=Path)
    parser.add_argument("--platform", choices=tuple(native.REQUIRED_LIBRARY_CANDIDATES), required=True)
    args = parser.parse_args()

    root = args.artifact_root.resolve()
    platform = args.platform
    if not root.is_dir():
        fail(f"artifact_root_not_found:{root}")
    for path in root.rglob("*"):
        if path.is_symlink():
            fail(f"symlink_forbidden:{path}")
    require_exact_directory_set(
        root, ROOT_DIRECTORIES, "native_root", allow_regular_files=True
    )
    require_exact_directory_set(root / "etc", {"scratchbird"}, "native_etc")
    require_exact_directory_set(root / "share", {"scratchbird"}, "native_share")

    try:
        standalone = json.loads(
            (root / "STANDALONE_OUTPUT_MANIFEST.json").read_text(encoding="utf-8")
        )
        profile = json.loads(
            (root / "NATIVE_RELEASE_PROFILE.json").read_text(encoding="utf-8")
        )
    except (FileNotFoundError, json.JSONDecodeError) as exc:
        fail(f"profile_metadata_invalid:{exc}")
    if standalone.get("platform") != platform:
        fail("standalone_platform_mismatch")
    if standalone.get("distribution_profile") != native.PROFILE_NAME:
        fail("standalone_native_profile_missing")
    if standalone.get("emulation_components") != "excluded":
        fail("standalone_emulation_exclusion_missing")
    if standalone.get("topology") != native.NATIVE_TOPOLOGY:
        fail("standalone_topology_contract_mismatch")
    standalone_runtime_requirements = standalone.get("runtime_requirements")
    if not isinstance(standalone_runtime_requirements, dict):
        fail("standalone_runtime_requirements_missing")
    standalone_llvm = native.require_llvm_runtime_contract(
        standalone_runtime_requirements.get("llvm"), platform
    )
    if profile.get("schema_id") != native.PROFILE_SCHEMA:
        fail("native_profile_schema_mismatch")
    if profile.get("profile") != native.PROFILE_NAME:
        fail("native_profile_name_mismatch")
    if profile.get("native_parser") != "SBSQL":
        fail("native_parser_identity_mismatch")
    if profile.get("emulation_components") != "excluded":
        fail("native_profile_emulation_exclusion_missing")
    if profile.get("topology") != native.NATIVE_TOPOLOGY:
        fail("native_profile_topology_contract_mismatch")
    profile_llvm = native.require_llvm_runtime_contract(
        profile.get("llvm_runtime"), platform
    )
    if profile_llvm != standalone_llvm:
        fail("native_profile_llvm_runtime_contract_mismatch")
    runtime_dependencies = set(profile.get("runtime_dependencies", []))
    if any(not native.safe_runtime_dependency_name(name) for name in runtime_dependencies):
        fail("native_profile_runtime_dependency_forbidden")
    if platform == "windows":
        native.require_windows_runtime_inventory(runtime_dependencies)
    llvm_runtime_library = str(profile_llvm["runtime_library"])
    if platform == "windows" and llvm_runtime_library not in runtime_dependencies:
        fail("windows_llvm_runtime_not_bundled")
    if platform != "windows" and llvm_runtime_library in runtime_dependencies:
        fail("external_llvm_runtime_must_not_be_bundled")

    expected_bins = {
        native.platform_executable(name, platform)
        for name in native.native_executables(platform)
    }
    allowed_bin_libraries = {
        name
        for candidates in native.REQUIRED_LIBRARY_CANDIDATES[platform].values()
        for name in candidates
        if name.endswith(".dll")
    }
    allowed_bin_libraries |= runtime_dependencies
    actual_bins = file_names(root / "bin")
    if not expected_bins <= actual_bins:
        fail(f"native_executable_missing:{sorted(expected_bins - actual_bins)}")
    unexpected_bins = actual_bins - expected_bins - allowed_bin_libraries
    if unexpected_bins:
        fail(f"non_native_executable_forbidden:{sorted(unexpected_bins)}")

    actual_libs = file_names(root / "lib")
    allowed_libs = {
        name
        for candidates in native.REQUIRED_LIBRARY_CANDIDATES[platform].values()
        for name in candidates
        if not name.endswith(".dll")
    }
    unexpected_libs = actual_libs - allowed_libs
    if unexpected_libs:
        fail(f"non_native_library_forbidden:{sorted(unexpected_libs)}")
    for label, candidates in native.REQUIRED_LIBRARY_CANDIDATES[platform].items():
        if not any(name in actual_bins or name in actual_libs for name in candidates):
            fail(f"required_native_library_missing:{label}:{','.join(candidates)}")

    config_root = root / "etc" / "scratchbird"
    actual_configs = file_names(config_root)
    expected_configs = set(native.NATIVE_CONFIGS)
    if actual_configs != expected_configs:
        fail(
            f"native_config_set_mismatch:missing={sorted(expected_configs - actual_configs)}:"
            f"unexpected={sorted(actual_configs - expected_configs)}"
        )
    native.require_native_configs(config_root, root, platform)
    resource_summary = native.require_operational_resources(root / "share")
    nonresource_inventory = native.require_native_share_layout(root / "share")
    if profile.get("required_resource_directories") != list(native.REQUIRED_RESOURCE_DIRS):
        fail("native_profile_resource_directory_contract_mismatch")
    if profile.get("required_resource_files") != list(native.REQUIRED_RESOURCE_FILES):
        fail("native_profile_resource_file_contract_mismatch")
    if profile.get("required_operability_files") != list(native.REQUIRED_OPERABILITY_FILES):
        fail("native_profile_operability_file_contract_mismatch")
    if profile.get("native_share_subtrees") != list(native.NATIVE_SHARE_SUBTREES):
        fail("native_profile_share_subtree_contract_mismatch")
    if profile.get("native_share_nonresource_inventory") != nonresource_inventory:
        fail("native_profile_nonresource_inventory_mismatch")
    if profile.get("resource_artifact_counts") != resource_summary["resource_artifact_counts"]:
        fail("native_profile_resource_artifact_counts_mismatch")
    if profile.get("policy_content_file_count") != resource_summary["policy_content_file_count"]:
        fail("native_profile_policy_content_count_mismatch")

    unexpected_root_files = {
        path.name for path in root.iterdir() if path.is_file()
    } - ROOT_METADATA
    if unexpected_root_files:
        fail(f"unexpected_root_metadata:{sorted(unexpected_root_files)}")
    print(f"verify_native_release_bundle=passed:{root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
