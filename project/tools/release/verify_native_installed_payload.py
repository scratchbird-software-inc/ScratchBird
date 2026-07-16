#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

"""Verify the extracted installer payload contains only the declared native SB set."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys

import stage_native_release_bundle as native

SYSTEM_CONFIG_FORBIDDEN_MARKERS = (
    "compatibility",
    "emulation",
    "firebird",
    "mysql",
    "postgres",
)
PORTABLE_SYSTEM_REWRITE_TOKENS = {
    "SBsrv.conf": (
        "data_dir = runtime/data",
        "control_dir = runtime/control",
        "log_file = stderr",
        "default_path = data/default.sbdb",
        "executable_path = bin/SBgate",
        "parser_executable_path = bin/SBParser",
        "control_dir = runtime/listener/control",
        "runtime_dir = runtime/listener/runtime",
        "sbps_endpoint = runtime/control/sb_server.sbps.sock",
    ),
    "SBgate.conf": (
        "parser_executable = bin/SBParser",
        "server_endpoint = runtime/control/sb_server.sbps.sock",
        "control_dir = runtime/listener/control",
        "runtime_dir = runtime/listener/runtime",
    ),
    "SBmgr.conf": (
        "manager.runtime_dir = runtime/manager/runtime",
        "manager.control_dir = runtime/manager/control",
        "manager.log.path = stderr",
        "manager.owner.database_path = data/default.sbdb",
    ),
    "SBParser.conf": ("parser.worker_binary = bin/SBParser",),
    "SBbootstrap.profile": (
        "platform = operator_required",
        "service_identity = operator_required",
        "service_group = operator_required",
    ),
}


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


def unique_tokens(tokens: list[str]) -> tuple[str, ...]:
    return tuple(dict.fromkeys(tokens))


def system_config_tokens(
    platform: str,
    mode: str,
    runtime_root: Path,
    config_root: Path,
) -> dict[str, tuple[str, ...]]:
    if mode == "system-defaults":
        if platform != "windows":
            fail(f"system_defaults_platform_invalid:{platform}")
        install_root = "@SCRATCHBIRD_INSTALL_ROOT@"
        state_root = "@SCRATCHBIRD_STATE_ROOT@"
        executable_suffix = ".exe"
    elif mode == "system-installed":
        if platform == "windows":
            install_root = runtime_root.as_posix()
            state_root = config_root.parent.as_posix()
            executable_suffix = ".exe"
        else:
            install_root = "/opt/ScratchBird"
            state_root = ""
            executable_suffix = ""
    else:
        fail(f"system_config_mode_invalid:{mode}")

    if platform == "linux":
        run_root = "/run/scratchbird"
        data_root = "/var/lib/scratchbird"
        log_root = "/var/log/scratchbird"
        server_runtime = f"{run_root}/runtime"
        server_control = f"{run_root}/control"
    elif platform == "macos":
        run_root = "/var/run/scratchbird"
        data_root = "/var/lib/scratchbird"
        log_root = "/var/log/scratchbird"
        server_runtime = f"{run_root}/sb_server"
        server_control = f"{run_root}/sb_server/control"
    elif platform == "windows":
        run_root = f"{state_root}/run"
        data_root = state_root
        log_root = f"{state_root}/log"
        server_runtime = f"{run_root}/sb_server"
        server_control = f"{run_root}/sb_server/control"
    else:
        fail(f"system_config_platform_invalid:{platform}")

    parser = f"{install_root}/bin/SBParser{executable_suffix}"
    return {
        "SBsrv.conf": (
            f"data_dir = {server_runtime}",
            f"control_dir = {server_control}",
            f"log_file = {log_root}/SBsrv.log",
            f"default_path = {data_root}/data/default.sbdb",
            f"executable_path = {install_root}/bin/SBgate{executable_suffix}",
            f"parser_executable_path = {parser}",
            f"control_dir = {run_root}/listener/control",
            f"runtime_dir = {run_root}/listener/runtime",
            f"sbps_endpoint = {server_control}/sb_server.sbps.sock",
        ),
        "SBgate.conf": (
            f"parser_executable = {parser}",
            f"server_endpoint = {server_control}/sb_server.sbps.sock",
            f"control_dir = {run_root}/listener/control",
            f"runtime_dir = {run_root}/listener/runtime",
        ),
        "SBmgr.conf": (
            f"manager.runtime_dir = {run_root}/manager/runtime",
            f"manager.control_dir = {run_root}/manager/control",
            f"manager.log.path = {log_root}/SBmgr.log",
            f"manager.owner.database_path = {data_root}/data/default.sbdb",
        ),
        "SBParser.conf": (f"parser.worker_binary = {parser}",),
        "SBbootstrap.profile": (
            f"platform = {platform}",
            "service_identity = "
            + (r"NT SERVICE\scratchbird" if platform == "windows" else "scratchbird"),
            "service_group = "
            + ("ScratchBird" if platform == "windows" else "scratchbird"),
        ),
    }


def require_system_configs(
    config_root: Path,
    platform: str,
    runtime_root: Path,
    mode: str,
) -> None:
    system_tokens = system_config_tokens(platform, mode, runtime_root, config_root)
    required: dict[str, tuple[str, ...]] = {}
    forbidden: dict[str, tuple[str, ...]] = {}
    for file_name, portable_tokens in native.REQUIRED_CONFIG_TOKENS.items():
        rewritten_tokens = PORTABLE_SYSTEM_REWRITE_TOKENS[file_name]
        path = native.require_regular_file(
            config_root / file_name, f"system_config:{file_name}"
        )
        text = path.read_text(encoding="utf-8").casefold()
        for marker in SYSTEM_CONFIG_FORBIDDEN_MARKERS:
            if marker in text:
                fail(f"system_config_forbidden_marker:{file_name}:{marker}")
        placeholders = set(re.findall(r"@[A-Z0-9_]+@", text.upper()))
        if mode == "system-defaults" and placeholders - {
            "@SCRATCHBIRD_INSTALL_ROOT@",
            "@SCRATCHBIRD_STATE_ROOT@",
        }:
            fail(f"system_config_unknown_placeholder:{file_name}")
        if mode == "system-installed" and placeholders:
            fail(f"system_config_unresolved_placeholder:{file_name}")
        required[file_name] = unique_tokens(
            [token for token in portable_tokens if token not in rewritten_tokens]
            + list(system_tokens[file_name])
        )
        forbidden[file_name] = rewritten_tokens
    native.require_native_configs(
        config_root,
        required_tokens=required,
        forbidden_tokens=forbidden,
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
    parser.add_argument(
        "--config-mode",
        choices=("portable", "system-defaults", "system-installed"),
        default="portable",
        help=(
            "Configuration layout contract. Portable mode uses packaged relative "
            "paths; system-defaults permits only Windows MSI materialization "
            "placeholders; system-installed requires final absolute paths."
        ),
    )
    args = parser.parse_args()
    if args.config_mode == "portable" and args.config_root is not None:
        fail("portable_config_root_must_be_implicit")
    if args.config_mode != "portable" and args.config_root is None:
        fail(f"explicit_config_root_required:{args.config_mode}")
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
        native.platform_executable(name, platform) for name in native.NATIVE_EXECUTABLES
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
    if args.config_mode == "portable":
        native.require_native_configs(config_root)
    else:
        require_system_configs(
            config_root,
            platform,
            runtime_root,
            args.config_mode,
        )
    print(f"verify_native_installed_payload=passed:{runtime_root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
