#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

"""Verify the extracted installer payload contains only the declared native SB set."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

import stage_native_release_bundle as native


def fail(message: str) -> None:
    print(f"verify_native_installed_payload=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def exactly_one(paths: list[Path], label: str) -> Path:
    if len(paths) != 1:
        fail(f"{label}_cardinality:{len(paths)}")
    return paths[0]


def regular_names(root: Path) -> set[str]:
    if not root.is_dir():
        fail(f"directory_missing:{root}")
    names: set[str] = set()
    for path in root.iterdir():
        if path.is_symlink() or not path.is_file():
            fail(f"non_regular_payload_entry:{path}")
        names.add(path.name)
    return names


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("payload_root", type=Path)
    parser.add_argument(
        "--expected-architecture",
        choices=("x86_64", "arm64", "universal"),
    )
    parser.add_argument(
        "--config-root",
        type=Path,
        help=(
            "Explicit installed/default config root. System packages may keep "
            "pristine defaults outside the portable /etc layout."
        ),
    )
    args = parser.parse_args()
    payload_root = args.payload_root.resolve()
    profiles = list(payload_root.rglob("NATIVE_RELEASE_PROFILE.json"))
    profile_path = exactly_one(profiles, "native_profile")
    try:
        profile = json.loads(profile_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        fail(f"native_profile_invalid:{exc}")
    platform = profile.get("platform")
    if platform not in native.REQUIRED_LIBRARY_CANDIDATES:
        fail(f"native_profile_platform_invalid:{platform}")
    if profile.get("schema_id") != native.PROFILE_SCHEMA:
        fail("native_profile_schema_mismatch")
    if profile.get("profile") != native.PROFILE_NAME:
        fail("native_profile_name_mismatch")
    if profile.get("native_parser") != "SBSQL":
        fail("native_parser_identity_mismatch")
    if profile.get("emulation_components") != "excluded":
        fail("emulation_exclusion_missing")
    llvm_runtime = native.require_llvm_runtime_contract(
        profile.get("llvm_runtime"), platform
    )
    llvm_runtime_paths = llvm_runtime.get("runtime_libraries_by_architecture")
    if platform == "macos" and args.expected_architecture == "universal":
        if not isinstance(llvm_runtime_paths, dict) or set(llvm_runtime_paths) != {
            "x86_64",
            "arm64",
        }:
            fail("macos_universal_llvm_runtime_map_required")
    elif platform == "macos" and args.expected_architecture in {"x86_64", "arm64"}:
        if llvm_runtime_paths is not None:
            fail("macos_per_architecture_llvm_runtime_must_be_scalar")
        expected_prefix = (
            "/usr/local/opt/llvm/lib/"
            if args.expected_architecture == "x86_64"
            else "/opt/homebrew/opt/llvm/lib/"
        )
        if not str(llvm_runtime.get("runtime_library", "")).startswith(
            expected_prefix
        ):
            fail(
                "macos_llvm_runtime_architecture_path_mismatch:"
                f"{args.expected_architecture}:{llvm_runtime.get('runtime_library')}"
            )
    if profile.get("required_resource_directories") != list(native.REQUIRED_RESOURCE_DIRS):
        fail("resource_directory_contract_mismatch")
    if profile.get("required_resource_files") != list(native.REQUIRED_RESOURCE_FILES):
        fail("resource_file_contract_mismatch")
    if profile.get("required_operability_files") != list(native.REQUIRED_OPERABILITY_FILES):
        fail("operability_file_contract_mismatch")
    if profile.get("native_share_subtrees") != list(native.NATIVE_SHARE_SUBTREES):
        fail("share_subtree_contract_mismatch")

    runtime_root = profile_path.parents[3]
    resource_summary = native.require_operational_resources(runtime_root / "share")
    native.require_native_share_layout(runtime_root / "share")
    if profile.get("resource_artifact_counts") != resource_summary["resource_artifact_counts"]:
        fail("resource_artifact_counts_mismatch")
    if profile.get("policy_content_file_count") != resource_summary["policy_content_file_count"]:
        fail("policy_content_count_mismatch")
    expected_executables = {
        native.platform_executable(name, platform)
        for name in native.native_executables(platform)
    }
    declared_executables = set(profile.get("executables", []))
    if declared_executables != expected_executables:
        fail("declared_native_executable_set_mismatch")
    declared_library_paths = set(profile.get("libraries", []))
    runtime_dependencies = set(profile.get("runtime_dependencies", []))
    if any(not native.safe_runtime_dependency_name(name) for name in runtime_dependencies):
        fail("declared_runtime_dependency_forbidden")
    llvm_runtime_library = str(llvm_runtime["runtime_library"])
    if platform == "windows" and llvm_runtime_library not in runtime_dependencies:
        fail("windows_llvm_runtime_not_bundled")
    if platform != "windows" and llvm_runtime_library in runtime_dependencies:
        fail("external_llvm_runtime_must_not_be_bundled")
    declared_bin_libraries = {
        Path(path).name for path in declared_library_paths if Path(path).parent.name == "bin"
    }
    declared_libraries = {
        Path(path).name for path in declared_library_paths if Path(path).parent.name == "lib"
    }
    allowed_library_names = {
        name
        for candidates in native.REQUIRED_LIBRARY_CANDIDATES[platform].values()
        for name in candidates
    } | native.OPTIONAL_NATIVE_LIBRARY_NAMES
    if (declared_bin_libraries | declared_libraries) - allowed_library_names:
        fail("declared_non_native_library_forbidden")
    declared_library_names = declared_bin_libraries | declared_libraries
    for role, candidates in native.REQUIRED_LIBRARY_CANDIDATES[platform].items():
        if not any(candidate in declared_library_names for candidate in candidates):
            fail(f"declared_required_native_library_missing:{role}")

    actual_bins = regular_names(runtime_root / "bin")
    expected_bins = expected_executables | declared_bin_libraries | runtime_dependencies
    if actual_bins != expected_bins:
        fail(
            f"installed_bin_set_mismatch:missing={sorted(expected_bins - actual_bins)}:"
            f"unexpected={sorted(actual_bins - expected_bins)}"
        )
    actual_libraries = regular_names(runtime_root / "lib")
    if actual_libraries != declared_libraries:
        fail(
            f"installed_library_set_mismatch:missing={sorted(declared_libraries - actual_libraries)}:"
            f"unexpected={sorted(actual_libraries - declared_libraries)}"
        )
    config_root = (
        args.config_root.resolve()
        if args.config_root is not None
        else runtime_root.parents[1] / "etc" / "scratchbird"
    )
    actual_configs = regular_names(config_root)
    expected_configs = set(native.NATIVE_CONFIGS)
    if actual_configs != expected_configs:
        fail(
            f"installed_config_set_mismatch:missing={sorted(expected_configs - actual_configs)}:"
            f"unexpected={sorted(actual_configs - expected_configs)}"
        )
    native.require_native_installed_configs(config_root, runtime_root, platform)
    print(f"verify_native_installed_payload=passed:{runtime_root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
