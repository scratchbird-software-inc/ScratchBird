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


def directory_names(root: Path) -> set[str]:
    """Return a closed directory inventory, rejecting files and links."""

    if not root.is_dir():
        fail(f"directory_missing:{root}")
    names: set[str] = set()
    for path in root.iterdir():
        if path.is_symlink() or not path.is_dir():
            fail(f"non_directory_payload_entry:{path}")
        names.add(path.name)
    return names


def require_exact_directory_set(root: Path, expected: set[str], label: str) -> None:
    actual = directory_names(root)
    if actual != expected:
        fail(
            f"{label}_directory_set_mismatch:"
            f"missing={sorted(expected - actual)}:"
            f"unexpected={sorted(actual - expected)}"
        )


def require_regular_tree(payload_root: Path) -> None:
    """Reject links and non-file/non-directory entries anywhere in a payload."""

    if not payload_root.is_dir():
        fail(f"payload_root_missing:{payload_root}")
    for path in payload_root.rglob("*"):
        if path.is_symlink() or not (path.is_file() or path.is_dir()):
            fail(f"payload_entry_type_forbidden:{path}")


def require_payload_envelope(
    payload_root: Path,
    runtime_root: Path,
    platform: str,
    system_payload: bool,
) -> None:
    """Require the entire extracted payload to have a named, closed layout.

    `runtime_root` checks alone are insufficient: an archive may carry a
    second client subtree beside the legitimate native runtime.  Directly
    installed system roots are supported as a distinct exact form; otherwise
    the portable payload must use the canonical ``opt/ScratchBird`` envelope.
    """

    try:
        runtime_relative = runtime_root.relative_to(payload_root)
    except ValueError:
        fail(f"runtime_root_outside_payload:{runtime_root}")
    if runtime_relative == Path("."):
        return

    allowed_relatives = {Path("opt") / "ScratchBird"}
    if platform == "windows":
        # Administrative MSI extraction commonly retains this Windows
        # directory envelope; it remains exact rather than wildcarded.
        allowed_relatives.add(Path("Program Files") / "ScratchBird")
    if runtime_relative not in allowed_relatives:
        fail(f"installed_runtime_location_forbidden:{runtime_relative.as_posix()}")

    if runtime_relative == Path("opt") / "ScratchBird":
        expected_top = {"opt", "etc"}
        if system_payload and platform == "linux":
            expected_top.add("usr")
        elif system_payload and platform == "macos":
            expected_top = {"opt", "Library"}
        require_exact_directory_set(payload_root, expected_top, "installed_payload_root")
        require_exact_directory_set(payload_root / "opt", {"ScratchBird"}, "installed_opt")
        if "etc" in expected_top:
            require_exact_directory_set(
                payload_root / "etc", {"scratchbird"}, "installed_etc"
            )
        if "usr" in expected_top:
            require_exact_directory_set(payload_root / "usr", {"lib"}, "installed_usr")
            require_exact_directory_set(
                payload_root / "usr" / "lib",
                {"scratchbird", "systemd", "sysusers.d", "tmpfiles.d"},
                "installed_usr_lib",
            )
        if "Library" in expected_top:
            require_exact_directory_set(
                payload_root / "Library", {"LaunchDaemons"}, "installed_library"
            )
        return

    # Windows administrative images retain a single Program Files envelope.
    require_exact_directory_set(
        payload_root, {"Program Files"}, "installed_payload_root"
    )
    require_exact_directory_set(
        payload_root / "Program Files", {"ScratchBird"}, "installed_program_files"
    )


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
    require_regular_tree(payload_root)
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
    release_root = runtime_root / "share" / "scratchbird" / "release"
    system_profile_by_platform = {
        "windows": "WINDOWS_SYSTEM_INSTALL_PROFILE.json",
        "macos": "MACOS_SYSTEM_INSTALL_PROFILE.json",
    }
    system_profile = system_profile_by_platform.get(platform)
    system_payload = system_profile is not None and (release_root / system_profile).is_file()
    require_payload_envelope(payload_root, runtime_root, platform, system_payload)
    expected_runtime_directories = {"bin", "lib", "share"}
    if system_payload:
        expected_runtime_directories.add("libexec")
    require_exact_directory_set(
        runtime_root, expected_runtime_directories, "installed_runtime_root"
    )
    require_exact_directory_set(
        runtime_root / "share", {"scratchbird"}, "installed_share"
    )
    if system_payload:
        expected_libexec = {
            "windows": {"scratchbird-windows-system-install.ps1"},
            "macos": {"scratchbird-macos-system-install"},
        }[platform]
        actual_libexec = regular_names(runtime_root / "libexec")
        if actual_libexec != expected_libexec:
            fail(
                "installed_system_libexec_set_mismatch:"
                f"missing={sorted(expected_libexec - actual_libexec)}:"
                f"unexpected={sorted(actual_libexec - expected_libexec)}"
            )
    resource_summary = native.require_operational_resources(runtime_root / "share")
    nonresource_inventory = native.require_native_share_layout(
        runtime_root / "share",
        allow_installed_release_metadata=True,
        allow_system_configuration_defaults=system_payload,
    )
    if profile.get("resource_artifact_counts") != resource_summary["resource_artifact_counts"]:
        fail("resource_artifact_counts_mismatch")
    if profile.get("policy_content_file_count") != resource_summary["policy_content_file_count"]:
        fail("policy_content_count_mismatch")
    if profile.get("native_share_nonresource_inventory") != nonresource_inventory:
        fail("nonresource_inventory_contract_mismatch")
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
    if platform == "windows":
        native.require_windows_runtime_inventory(runtime_dependencies)
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
    }
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
