#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

"""Static and fixture-root conformance smoke for the macOS system installer."""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import shutil
import stat
import subprocess
import sys
from typing import Any
from types import ModuleType


SERVICE_USER = "scratchbird"
SERVICE_GROUP = "scratchbird"
NATIVE_PORT = 3092
CONFIG_NAMES = (
    "SBsrv.conf",
    "SBgate.conf",
    "SBmgr.conf",
    "SBParser.conf",
    "SBbootstrap.profile",
)
REQUIRED_DIRECTORIES = (
    "Library/Application Support/ScratchBird",
    "var/lib/scratchbird",
    "var/lib/scratchbird/data",
    "var/lib/scratchbird/install",
    "var/log/scratchbird",
    "var/run/scratchbird",
    "var/run/scratchbird/sb_server",
    "var/run/scratchbird/sb_server/control",
    "var/run/scratchbird/listener",
    "var/run/scratchbird/listener/control",
    "var/run/scratchbird/listener/runtime",
    "var/run/scratchbird/manager",
    "var/run/scratchbird/manager/control",
    "var/run/scratchbird/manager/runtime",
)
CONFIG_REPLACEMENTS: dict[str, dict[str, str]] = {
    "SBsrv.conf": {
        "data_dir = runtime/data": "data_dir = /var/run/scratchbird/sb_server",
        "control_dir = runtime/control": (
            "control_dir = /var/run/scratchbird/sb_server/control"
        ),
        "log_file = stderr": "log_file = /var/log/scratchbird/SBsrv.log",
        "default_path = data/default.sbdb": (
            "default_path = /var/lib/scratchbird/data/default.sbdb"
        ),
        "executable_path = bin/SBgate": (
            "executable_path = /opt/ScratchBird/bin/SBgate"
        ),
        "parser_executable_path = bin/SBParser": (
            "parser_executable_path = /opt/ScratchBird/bin/SBParser"
        ),
        "control_dir = runtime/listener/control": (
            "control_dir = /var/run/scratchbird/listener/control"
        ),
        "runtime_dir = runtime/listener/runtime": (
            "runtime_dir = /var/run/scratchbird/listener/runtime"
        ),
        "sbps_endpoint = runtime/control/sb_server.sbps.sock": (
            "sbps_endpoint = /var/run/scratchbird/sb_server/control/"
            "sb_server.sbps.sock"
        ),
    },
    "SBgate.conf": {
        "parser_executable = bin/SBParser": (
            "parser_executable = /opt/ScratchBird/bin/SBParser"
        ),
        "server_endpoint = runtime/control/sb_server.sbps.sock": (
            "server_endpoint = /var/run/scratchbird/sb_server/control/"
            "sb_server.sbps.sock"
        ),
        "control_dir = runtime/listener/control": (
            "control_dir = /var/run/scratchbird/listener/control"
        ),
        "runtime_dir = runtime/listener/runtime": (
            "runtime_dir = /var/run/scratchbird/listener/runtime"
        ),
    },
    "SBmgr.conf": {
        "manager.runtime_dir = runtime/manager/runtime": (
            "manager.runtime_dir = /var/run/scratchbird/manager/runtime"
        ),
        "manager.control_dir = runtime/manager/control": (
            "manager.control_dir = /var/run/scratchbird/manager/control"
        ),
        "manager.log.path = stderr": (
            "manager.log.path = /var/log/scratchbird/SBmgr.log"
        ),
        "manager.owner.database_path = data/default.sbdb": (
            "manager.owner.database_path = /var/lib/scratchbird/data/default.sbdb"
        ),
    },
    "SBParser.conf": {
        "parser.worker_binary = bin/SBParser": (
            "parser.worker_binary = /opt/ScratchBird/bin/SBParser"
        ),
    },
}


class SmokeFailure(RuntimeError):
    pass


def fail(code: str) -> None:
    raise SmokeFailure(code)


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def require_mode(path: Path, expected: int) -> None:
    actual = stat.S_IMODE(path.stat().st_mode)
    if actual != expected:
        fail(f"fixture_mode_mismatch:{path.name}:{actual:o}:{expected:o}")


def write_system_config_defaults(repo_root: Path, fixture_root: Path) -> None:
    source_root = repo_root / "project" / "config" / "templates"
    target_root = (
        fixture_root
        / "opt"
        / "ScratchBird"
        / "share"
        / "scratchbird"
        / "config-defaults"
    )
    target_root.mkdir(parents=True)
    for name in CONFIG_NAMES:
        source = source_root / name
        if not source.is_file():
            fail(f"source_config_missing:{name}")
        text = source.read_text(encoding="utf-8")
        for old, new in CONFIG_REPLACEMENTS.get(name, {}).items():
            if text.count(old) != 1:
                fail(f"source_config_assignment_mismatch:{name}:{old}")
            text = text.replace(old, new, 1)
        (target_root / name).write_text(text, encoding="utf-8")


def validate_profile(asset_root: Path) -> dict[str, Any]:
    profile_path = asset_root / "MACOS_SYSTEM_INSTALL_PROFILE.json"
    try:
        profile = json.loads(profile_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        fail(f"profile_invalid:{exc.__class__.__name__}")
    if profile.get("schema_id") != "scratchbird.macos_system_install_profile.v1":
        fail("profile_schema_mismatch")
    if profile.get("distribution_profile") != "native-sbsql-only":
        fail("profile_distribution_mismatch")
    if profile.get("native_default_port") != NATIVE_PORT:
        fail("profile_native_port_mismatch")
    if profile.get("database_files_created") is not False:
        fail("profile_database_creation_not_false")
    if profile.get("security_sidecars_created") is not False:
        fail("profile_sidecar_creation_not_false")
    if profile.get("create_time_os_authorization") != "root_only":
        fail("profile_create_authority_not_root_only")
    if profile.get("human_service_group_membership_mutation") != "forbidden":
        fail("profile_human_membership_mutation_not_forbidden")

    identity = profile.get("os_identity", {})
    if identity.get("service_user") != SERVICE_USER:
        fail("profile_service_user_mismatch")
    if identity.get("group") != SERVICE_GROUP:
        fail("profile_service_group_mismatch")
    if identity.get("service_login") != "forbidden":
        fail("profile_service_login_not_forbidden")
    if identity.get("service_hidden") is not True:
        fail("profile_service_identity_not_hidden")
    if identity.get("service_administrator_group_membership") != "forbidden":
        fail("profile_service_admin_membership_not_forbidden")
    if identity.get("service_explicit_supplementary_group_membership") != (
        "forbidden_except_scratchbird"
    ):
        fail("profile_service_supplementary_membership_not_forbidden")
    if identity.get("service_group_nested_in_other_local_group") != "forbidden":
        fail("profile_service_group_nesting_not_forbidden")
    if identity.get("service_implicit_baseline_groups") != (
        "macos_computed_everyone_and_localaccounts_are_not_explicit_membership"
    ):
        fail("profile_service_implicit_baseline_policy_mismatch")
    if identity.get("service_resolved_effective_group_set") != (
        "primary_scratchbird_plus_macos_implicit_gid_12_and_61_only"
    ):
        fail("profile_service_resolved_group_policy_mismatch")
    if identity.get("service_authority_scope") != (
        "filesystem_directory_and_process_execution_only_"
        "no_database_or_security_authority"
    ):
        fail("profile_service_authority_scope_mismatch")
    if "transitive dseditgroup checkmember" not in identity.get(
        "service_administrator_membership_validation", ""
    ):
        fail("profile_service_transitive_admin_check_missing")
    uid_policy = identity.get("uid_policy", {})
    if uid_policy.get("minimum_inclusive") != 501:
        fail("profile_service_uid_minimum_mismatch")
    if uid_policy.get("maximum_exclusive") != 60000:
        fail("profile_service_uid_maximum_mismatch")
    if uid_policy.get("apple_reserved_uid_range_excluded") != "0-500":
        fail("profile_service_uid_reserved_range_guard_missing")
    if uid_policy.get("allocation") != "first_locally_unused_uid":
        fail("profile_service_uid_allocation_mismatch")
    if identity.get("package_installer_root_is_never_inferred_or_added") is not True:
        fail("profile_root_membership_guard_missing")

    service = profile.get("service", {})
    if service.get("default_enablement") != "disabled":
        fail("profile_service_not_disabled")
    if service.get("default_activity") != "not_started":
        fail("profile_service_not_inactive")
    if service.get("run_at_load") is not False:
        fail("profile_service_run_at_load")
    labels = service.get("launchd_labels")
    if labels != ["com.scratchbird.sbsrv", "com.scratchbird.sbmgr"]:
        fail("profile_launchd_service_set_mismatch")

    topology = profile.get("topology", {})
    if topology.get("native_route") != (
        "SBsrv_to_shared_SBgate_to_standalone_SBParser_to_IPC_engine"
    ):
        fail("profile_native_topology_mismatch")
    if topology.get("top_level_native_service") != "SBsrv":
        fail("profile_top_level_native_service_mismatch")
    if topology.get("manager") != "optional_SBmgr_before_listener":
        fail("profile_optional_manager_contract_mismatch")
    if topology.get("parser_process_model") != (
        "one_standalone_parser_per_emulation_or_native_dialect"
    ):
        fail("profile_standalone_parser_process_model_mismatch")
    profile_paths = {
        row.get("path")
        for row in profile.get("directories", [])
        if isinstance(row, dict)
    }
    for rel in REQUIRED_DIRECTORIES:
        absolute = "/" + rel
        if rel in {
            "var/run/scratchbird",
            "var/run/scratchbird/sb_server",
            "var/run/scratchbird/listener",
            "var/run/scratchbird/manager",
        }:
            continue
        if absolute not in profile_paths:
            fail(f"profile_directory_missing:{absolute}")
    return profile


def validate_launchd(asset_root: Path) -> dict[str, Any]:
    plist_paths = sorted(asset_root.glob("com.scratchbird.*.plist"))
    if [path.name for path in plist_paths] != [
        "com.scratchbird.sbmgr.plist",
        "com.scratchbird.sbsrv.plist",
    ]:
        fail("launchd_top_level_service_set_mismatch")
    rows: list[dict[str, Any]] = []
    for path in plist_paths:
        try:
            payload = plistlib.loads(path.read_bytes())
        except (OSError, plistlib.InvalidFileException) as exc:
            fail(f"launchd_plist_invalid:{path.name}:{exc.__class__.__name__}")
        label = payload.get("Label")
        expected_label = path.name.removesuffix(".plist")
        if label != expected_label:
            fail(f"launchd_label_mismatch:{path.name}")
        if payload.get("UserName") != SERVICE_USER:
            fail(f"launchd_user_mismatch:{path.name}")
        if payload.get("GroupName") != SERVICE_GROUP:
            fail(f"launchd_group_mismatch:{path.name}")
        if payload.get("RunAtLoad") is not False:
            fail(f"launchd_run_at_load:{path.name}")
        if payload.get("KeepAlive") is not False:
            fail(f"launchd_keep_alive:{path.name}")
        if payload.get("Disabled") is not True:
            fail(f"launchd_not_disabled:{path.name}")
        arguments = payload.get("ProgramArguments")
        if not isinstance(arguments, list) or "--config" not in arguments:
            fail(f"launchd_arguments_invalid:{path.name}")
        expected_executable = {
            "com.scratchbird.sbsrv": "/opt/ScratchBird/bin/SBsrv",
            "com.scratchbird.sbmgr": "/opt/ScratchBird/bin/SBmgr",
        }[label]
        if not arguments or arguments[0] != expected_executable:
            fail(f"launchd_top_level_executable_mismatch:{path.name}")
        expected_config = {
            "com.scratchbird.sbsrv": (
                "/Library/Application Support/ScratchBird/SBsrv.conf"
            ),
            "com.scratchbird.sbmgr": (
                "/Library/Application Support/ScratchBird/SBmgr.conf"
            ),
        }[label]
        config_index = arguments.index("--config")
        if (
            config_index + 1 >= len(arguments)
            or arguments[config_index + 1] != expected_config
        ):
            fail(f"launchd_config_path_mismatch:{path.name}")
        if "--create-if-missing" in arguments:
            fail(f"launchd_database_creation_forbidden:{path.name}")
        if any("SBgate" in str(value) or "SBParser" in str(value) for value in arguments):
            fail(f"launchd_child_process_service_forbidden:{path.name}")
        rows.append({"label": label, "arguments": arguments})
    return {"services": rows, "default_state": "disabled_unloaded_not_run_at_load"}


def validate_helper_static(helper: Path) -> None:
    if not helper.is_file() or not os.access(helper, os.X_OK):
        fail("lifecycle_helper_missing_or_not_executable")
    text = helper.read_text(encoding="utf-8")
    required = (
        "SERVICE_USER=scratchbird",
        "SERVICE_GROUP=scratchbird",
        "SERVICE_UID_MIN=501",
        "SERVICE_UID_MAX_EXCLUSIVE=60000",
        "ensure_service_is_not_admin",
        "ensure_service_has_no_explicit_supplementary_membership",
        "ensure_service_resolved_group_set_is_least_authority",
        "local_group_has_guid_member",
        "local_group_nests_group_guid",
        "GroupMembers",
        "NestedGroups",
        "/Groups/admin",
        'dseditgroup -n . -o checkmember',
        'id -G "$SERVICE_USER"',
        "12|61",
        "dscl . -create",
        "dseditgroup -o edit",
        '"/Library/Application Support/ScratchBird"',
        "/var/run/scratchbird/sb_server/control",
        'printf \'  "native_default_port": 3092',
        'printf \'  "database_files_created": false',
        'printf \'  "security_sidecars_created": false',
    )
    for fragment in required:
        if fragment not in text:
            fail(f"lifecycle_contract_missing:{fragment}")
    for forbidden in (
        "SUDO_USER",
        "${USER",
        "$USER",
        "LOGNAME",
        "launchctl load",
        "launchctl bootstrap",
        "launchctl enable",
        "launchctl start",
        "SBsec",
        "SBsrv",
        "--create-if-missing",
        ".sbdb",
        ".sbrd",
        "security_principal_events",
        "local_password_auth",
        "--installer-user",
        "GROUP_MEMBERSHIP_REQUIRED",
    ):
        if forbidden in text:
            fail(f"lifecycle_forbidden_fragment:{forbidden}")
    for forbidden_port in (3050, 3090, 3392):
        if str(forbidden_port) in text:
            fail(f"lifecycle_forbidden_native_port:{forbidden_port}")


def validate_pkg_scripts(asset_root: Path) -> dict[str, Any]:
    postinstall = asset_root / "pkg-scripts" / "postinstall.in"
    if not postinstall.is_file():
        fail("pkg_postinstall_template_missing")
    text = postinstall.read_text(encoding="utf-8")
    required = (
        "/opt/ScratchBird/libexec/scratchbird-macos-system-install",
        "--identity-mode system",
        "--package-format pkg",
        "@SCRATCHBIRD_VERSION@",
    )
    for fragment in required:
        if fragment not in text:
            fail(f"pkg_postinstall_contract_missing:{fragment}")
    for forbidden in (
        "SUDO_USER",
        "${USER",
        "$USER",
        "LOGNAME",
        "launchctl load",
        "launchctl bootstrap",
        "launchctl enable",
        "launchctl start",
    ):
        if forbidden in text:
            fail(f"pkg_postinstall_forbidden_fragment:{forbidden}")
    return {
        "postinstall_template": "present",
        "human_service_group_membership_mutation": "forbidden",
        "service_activation": "not_performed",
    }


def lifecycle_command(
    helper: Path,
    fixture_root: Path,
    *,
    action: str = "post-install",
) -> list[str]:
    return [
        str(helper),
        action,
        "--root",
        str(fixture_root.resolve()),
        "--identity-mode",
        "fixture",
        "--package-format",
        "fixture",
        "--package-version",
        "0.0.0-smoke",
    ]


def file_inventory(root: Path) -> set[str]:
    return {
        path.relative_to(root).as_posix()
        for path in root.rglob("*")
        if path.is_file()
    }


def reject_created_database_or_sidecar(before: set[str], after: set[str]) -> None:
    created = after - before
    forbidden_markers = (
        ".sbdb",
        ".sb.security_principal_events",
        ".sb.local_password_auth",
        "security_principal_events",
        "local_password_auth",
    )
    for rel in sorted(created):
        lowered = rel.lower()
        if any(marker in lowered for marker in forbidden_markers):
            fail(f"lifecycle_created_database_or_security_sidecar:{rel}")


def validate_installed_configs(fixture_root: Path) -> None:
    config_root = fixture_root / "Library" / "Application Support" / "ScratchBird"
    if (fixture_root / "etc" / "scratchbird").exists():
        fail("duplicate_noncanonical_live_config_root")
    validate_config_tree(config_root)


def validate_config_tree(config_root: Path, *, expected_mode: int = 0o640) -> None:
    actual_names = {
        path.name
        for path in config_root.iterdir()
        if path.is_file() and not path.is_symlink()
    }
    if actual_names != set(CONFIG_NAMES):
        fail("config_set_mismatch")
    for name in CONFIG_NAMES:
        path = config_root / name
        if not path.is_file():
            fail(f"installed_config_missing:{name}")
        require_mode(path, expected_mode)
    server = (config_root / "SBsrv.conf").read_text(encoding="utf-8")
    listener = (config_root / "SBgate.conf").read_text(encoding="utf-8")
    manager = (config_root / "SBmgr.conf").read_text(encoding="utf-8")
    parser = (config_root / "SBParser.conf").read_text(encoding="utf-8")
    required_fragments = (
        (server, "port = 3092"),
        (server, "default_path = /var/lib/scratchbird/data/default.sbdb"),
        (server, "executable_path = /opt/ScratchBird/bin/SBgate"),
        (server, "parser_executable_path = /opt/ScratchBird/bin/SBParser"),
        (
            server,
            "sbps_endpoint = /var/run/scratchbird/sb_server/control/"
            "sb_server.sbps.sock",
        ),
        (listener, "port = 3092"),
        (listener, "parser_executable = /opt/ScratchBird/bin/SBParser"),
        (manager, "manager.proxy.port = 3092"),
        (manager, "manager.backend.native_port = 0"),
        (parser, "parser.worker_binary = /opt/ScratchBird/bin/SBParser"),
    )
    for content, fragment in required_fragments:
        if fragment not in content:
            fail(f"installed_config_contract_missing:{fragment}")
    combined = "\n".join((server, listener, manager, parser))
    for forbidden_port in (3050, 3090, 3392):
        if str(forbidden_port) in combined:
            fail(f"installed_config_forbidden_native_port:{forbidden_port}")


def load_builder(repo_root: Path) -> ModuleType:
    path = repo_root / "project" / "tools" / "installers" / "build_installers.py"
    spec = importlib.util.spec_from_file_location("scratchbird_installer_builder", path)
    if spec is None or spec.loader is None:
        fail("builder_import_spec_failed")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def validate_builder_integration(
    repo_root: Path,
    work_root: Path,
) -> dict[str, Any]:
    builder = load_builder(repo_root)
    portable_root = work_root / "builder-portable"
    system_root = work_root / "builder-system"
    scripts_root = work_root / "builder-pkg-scripts"
    portable_config = portable_root / "etc" / "scratchbird"
    portable_config.mkdir(parents=True)
    source_config = repo_root / "project" / "config" / "templates"
    for name in CONFIG_NAMES:
        shutil.copy2(source_config / name, portable_config / name)

    builder.stage_macos_system_install_tree(
        portable_root,
        system_root,
        "0.0.0-smoke",
        "macos-system-fixture",
    )
    builder.materialize_macos_pkg_scripts(scripts_root, "0.0.0-smoke")

    if not portable_config.is_dir():
        fail("builder_mutated_portable_config_root")
    if (portable_root / "Library" / "Application Support" / "ScratchBird").exists():
        fail("builder_mutated_portable_with_system_config")
    if (system_root / "etc" / "scratchbird").exists():
        fail("builder_system_duplicate_etc_config_root")

    defaults_root = (
        system_root
        / "opt"
        / "ScratchBird"
        / "share"
        / "scratchbird"
        / "config-defaults"
    )
    validate_config_tree(defaults_root, expected_mode=0o644)
    bootstrap = (defaults_root / "SBbootstrap.profile").read_text(encoding="utf-8")
    for fragment in (
        "platform = macos",
        "service_identity = scratchbird",
        "service_group = scratchbird",
    ):
        if fragment not in bootstrap:
            fail(f"builder_bootstrap_profile_contract_missing:{fragment}")

    release_root = (
        system_root / "opt" / "ScratchBird" / "share" / "scratchbird" / "release"
    )
    profile = json.loads(
        (release_root / "MACOS_SYSTEM_INSTALL_PROFILE.json").read_text(
            encoding="utf-8"
        )
    )
    if profile.get("version") != "0.0.0-smoke":
        fail("builder_system_profile_version_mismatch")
    if profile.get("build_id") != "macos-system-fixture":
        fail("builder_system_profile_build_id_mismatch")
    install_manifest = json.loads(
        (release_root / "INSTALL_MANIFEST.json").read_text(encoding="utf-8")
    )
    if install_manifest.get("install_roots", {}).get("configuration") != (
        "/Library/Application Support/ScratchBird"
    ):
        fail("builder_install_manifest_config_root_mismatch")
    launchd_manifest = json.loads(
        (release_root / "MACOS_LAUNCHD_MANIFEST.json").read_text(encoding="utf-8")
    )
    if launchd_manifest.get("default_service_state") != (
        "installed_disabled_unloaded_not_run_at_load"
    ):
        fail("builder_launchd_manifest_default_state_mismatch")

    staged_helper = (
        system_root
        / "opt"
        / "ScratchBird"
        / "libexec"
        / "scratchbird-macos-system-install"
    )
    if not staged_helper.is_file() or not os.access(staged_helper, os.X_OK):
        fail("builder_lifecycle_helper_not_staged")
    postinstall = scripts_root / "postinstall"
    if not postinstall.is_file() or not os.access(postinstall, os.X_OK):
        fail("builder_pkg_postinstall_not_materialized")
    postinstall_text = postinstall.read_text(encoding="utf-8")
    if "@SCRATCHBIRD_VERSION@" in postinstall_text:
        fail("builder_pkg_postinstall_version_token_not_replaced")
    if "--package-version '0.0.0-smoke'" not in postinstall_text:
        fail("builder_pkg_postinstall_version_mismatch")

    package_output_root = work_root / "builder-package-output"
    package_output_root.mkdir()
    captured_command: list[str] = []
    original_which = builder.shutil.which
    original_run = builder.run

    def fixture_which(name: str) -> str | None:
        return "/usr/bin/pkgbuild" if name == "pkgbuild" else None

    def fixture_run(command: list[str], cwd: Path | None = None) -> str:
        del cwd
        captured_command.extend(command)
        Path(command[-1]).write_bytes(b"fixture-pkg\n")
        return "fixture-pkgbuild"

    try:
        builder.shutil.which = fixture_which
        builder.run = fixture_run
        package = builder.make_macos_pkg(
            system_root,
            package_output_root,
            "0.0.0-smoke",
            {"release_signing_enabled": False},
            scripts_root,
        )
    finally:
        builder.shutil.which = original_which
        builder.run = original_run
    if not package.is_file():
        fail("builder_pkg_command_did_not_produce_fixture")
    if "--scripts" not in captured_command or str(scripts_root) not in captured_command:
        fail("builder_pkg_scripts_option_missing")
    if "--ownership" not in captured_command or "recommended" not in captured_command:
        fail("builder_pkg_ownership_option_missing")
    if str(system_root) not in captured_command:
        fail("builder_pkg_system_payload_root_missing")

    evidence_path = builder.write_macos_system_package_evidence(
        package_output_root,
        "0.0.0-smoke",
        "macos-system-fixture",
    )
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    if evidence.get("native_default_port") != NATIVE_PORT:
        fail("builder_system_evidence_native_port_mismatch")
    if evidence.get("configuration", {}).get("live_root") != (
        "/Library/Application Support/ScratchBird"
    ):
        fail("builder_system_evidence_config_root_mismatch")
    if evidence.get("os_identity", {}).get("service_authority_scope") != (
        "filesystem_directory_and_process_execution_only_"
        "no_database_or_security_authority"
    ):
        fail("builder_system_evidence_service_authority_scope_mismatch")

    staged_plists = sorted(
        (system_root / "Library" / "LaunchDaemons").glob(
            "com.scratchbird.*.plist"
        )
    )
    if [path.name for path in staged_plists] != [
        "com.scratchbird.sbmgr.plist",
        "com.scratchbird.sbsrv.plist",
    ]:
        fail("builder_launchd_service_set_mismatch")
    for path in staged_plists:
        payload = plistlib.loads(path.read_bytes())
        if payload.get("UserName") != SERVICE_USER:
            fail(f"builder_launchd_user_mismatch:{path.name}")
        if payload.get("GroupName") != SERVICE_GROUP:
            fail(f"builder_launchd_group_mismatch:{path.name}")
        if payload.get("Disabled") is not True:
            fail(f"builder_launchd_not_disabled:{path.name}")

    return {
        "portable_config_layout_preserved": True,
        "system_config_root": "/Library/Application Support/ScratchBird",
        "five_native_configs_staged": True,
        "pkg_scripts_materialized": True,
        "pkgbuild_command_contract": True,
        "system_package_evidence": True,
        "launchd_system_assets_staged": True,
        "native_default_port": NATIVE_PORT,
    }


def run_fixture_smoke(
    repo_root: Path, helper: Path, work_root: Path
) -> dict[str, Any]:
    fixture_root = work_root / "fixture"
    fixture_root.mkdir(parents=True)
    write_system_config_defaults(repo_root, fixture_root)
    before = file_inventory(fixture_root)
    first = run(lifecycle_command(helper, fixture_root))
    if first.returncode != 0:
        fail(f"fixture_post_install_failed:{first.returncode}:{first.stderr.strip()}")
    if first.stderr:
        fail(f"fixture_unexpected_diagnostic:{first.stderr.strip()}")
    after_first = file_inventory(fixture_root)
    reject_created_database_or_sidecar(before, after_first)

    for rel in REQUIRED_DIRECTORIES:
        path = fixture_root / rel
        if not path.is_dir():
            fail(f"fixture_directory_missing:{rel}")
        require_mode(path, 0o750)
    validate_installed_configs(fixture_root)

    identity_root = (
        fixture_root
        / "var"
        / "lib"
        / "scratchbird"
        / "install"
        / "fixture-identities"
    )
    group = json.loads((identity_root / "group.json").read_text(encoding="utf-8"))
    service = json.loads((identity_root / "service.json").read_text(encoding="utf-8"))
    if group != {"name": SERVICE_GROUP, "kind": "local_group"}:
        fail("fixture_group_identity_mismatch")
    if (
        service.get("name") != SERVICE_USER
        or service.get("kind") != "non_login_service"
        or service.get("hidden") is not True
        or service.get("administrator_group_membership") is not False
    ):
        fail("fixture_service_identity_mismatch")

    state_path = (
        fixture_root
        / "var"
        / "lib"
        / "scratchbird"
        / "install"
        / "MACOS_SYSTEM_INSTALL_STATE.json"
    )
    state = json.loads(state_path.read_text(encoding="utf-8"))
    expected_state = {
        "service_user": SERVICE_USER,
        "service_group": SERVICE_GROUP,
        "service_uid_policy": "locally_unique_501_through_59999",
        "service_administrator_group_membership": False,
        "service_authority_scope": (
            "filesystem_directory_and_process_execution_only_"
            "no_database_or_security_authority"
        ),
        "resolved_effective_group_policy": (
            "primary_scratchbird_plus_macos_implicit_gid_12_and_61_only"
        ),
        "human_service_group_membership_mutated": False,
        "create_time_os_authorization": "root_only",
        "service_enablement_default": "disabled",
        "service_activity_default": "not_started",
        "launchd_load_performed": False,
        "native_default_port": NATIVE_PORT,
        "database_files_created": False,
        "security_sidecars_created": False,
    }
    for key, expected in expected_state.items():
        if state.get(key) != expected:
            fail(f"fixture_state_mismatch:{key}")

    config_root = fixture_root / "Library" / "Application Support" / "ScratchBird"
    server_config = config_root / "SBsrv.conf"
    server_config.write_text(
        server_config.read_text(encoding="utf-8") + "operator_override = preserved\n",
        encoding="utf-8",
    )
    custom_config = config_root / "operator-local.conf"
    custom_config.write_text("operator_local = preserved\n", encoding="utf-8")
    data_file = fixture_root / "var" / "lib" / "scratchbird" / "data" / "operator.db"
    data_file.write_text("operator-data\n", encoding="utf-8")
    before_repeat = file_inventory(fixture_root)
    repeat = run(lifecycle_command(helper, fixture_root))
    if repeat.returncode != 0:
        fail(f"fixture_repeat_failed:{repeat.returncode}:{repeat.stderr.strip()}")
    if "operator_override = preserved" not in server_config.read_text(encoding="utf-8"):
        fail("fixture_upgrade_overwrote_server_config")
    if custom_config.read_text(encoding="utf-8") != "operator_local = preserved\n":
        fail("fixture_upgrade_overwrote_operator_config")
    if data_file.read_text(encoding="utf-8") != "operator-data\n":
        fail("fixture_upgrade_overwrote_operator_data")
    reject_created_database_or_sidecar(before_repeat, file_inventory(fixture_root))

    rejected_installer_user = run(
        [*lifecycle_command(helper, fixture_root), "--installer-user", "root"]
    )
    if rejected_installer_user.returncode == 0:
        fail("fixture_installer_user_surface_accepted")
    if rejected_installer_user.stderr != "BOOTSTRAP.INSTALL_DEFAULTS_INVALID\n":
        fail("fixture_installer_user_surface_rejection_not_code_only")

    pre_remove = run(
        lifecycle_command(
            helper,
            fixture_root,
            action="pre-remove",
        )
    )
    if pre_remove.returncode != 0:
        fail(f"fixture_pre_remove_failed:{pre_remove.returncode}:{pre_remove.stderr.strip()}")
    preserved = (
        fixture_root
        / "var"
        / "lib"
        / "scratchbird"
        / "install"
        / "config-preserve"
    )
    if not (preserved / "operator-local.conf").is_file():
        fail("fixture_pre_remove_config_not_preserved")
    if not data_file.is_file():
        fail("fixture_pre_remove_data_not_preserved")

    return {
        "post_install": "passed",
        "idempotence": "passed",
        "upgrade_config_and_data_preservation": "passed",
        "pre_remove_preservation": "passed",
        "installer_user_surface_refusal": "passed",
        "human_service_group_membership_mutation": "forbidden",
        "code_only_diagnostics": "passed",
        "native_default_port": NATIVE_PORT,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-root", type=Path, required=True)
    parser.add_argument(
        "--asset-root",
        type=Path,
        default=Path(__file__).resolve().parent / "macos",
    )
    args = parser.parse_args()

    work_root = args.work_root.resolve()
    asset_root = args.asset_root.resolve()
    repo_root = Path(__file__).resolve().parents[3]
    if work_root.exists():
        shutil.rmtree(work_root)
    work_root.mkdir(parents=True)

    helper = asset_root / "scratchbird-macos-system-install.sh"
    proof: dict[str, Any] = {
        "schema_id": "scratchbird.macos_system_install_smoke.v1",
        "profile": validate_profile(asset_root),
        "launchd": validate_launchd(asset_root),
        "pkg_scripts": validate_pkg_scripts(asset_root),
    }
    validate_helper_static(helper)
    proof["builder_integration"] = validate_builder_integration(
        repo_root,
        work_root,
    )
    proof["lifecycle_fixture"] = run_fixture_smoke(
        repo_root, helper, work_root
    )
    proof["result"] = "passed"
    proof_path = work_root / "MACOS_SYSTEM_INSTALL_SMOKE.json"
    proof_path.write_text(
        json.dumps(proof, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(f"smoke_install_macos_system=passed:{proof_path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except SmokeFailure as exc:
        print(f"smoke_install_macos_system=fail:{exc}", file=sys.stderr)
        raise SystemExit(1)
