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
import re
import shutil
import stat
import subprocess
import sys
from typing import Any
from types import ModuleType


SERVICE_USER = "scratchbird"
SERVICE_GROUP = "scratchbird"
NATIVE_PORT = 3092
MACOS_SERVICE_PROCESS_GROUP_POLICY = (
    "launchd_host_computed_groups_cleared_before_scratchbird_product_exec"
)
MACOS_SERVICE_LAUNCHER = "/opt/ScratchBird/bin/SBlaunch"
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
    "var/log/scratchbird/launchd",
    "var/log/scratchbird/runtime",
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
        "log_file = stderr": (
            "log_file = /var/log/scratchbird/runtime/SBsrv.log"
        ),
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
            "manager.log.path = /var/log/scratchbird/runtime/SBmgr.log"
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
    lifecycle = profile.get("lifecycle", {})
    if lifecycle.get("package_preinstall") != "preinstall":
        fail("profile_pkg_preinstall_missing")
    if lifecycle.get("package_preinstall_existing_topology_policy") != (
        "reject_unsafe_existing_root_helper_launcher_and_plist_paths_before_"
        "payload"
    ):
        fail("profile_pkg_preinstall_topology_policy_mismatch")
    if lifecycle.get("package_postinstall") != "postinstall":
        fail("profile_pkg_postinstall_missing")
    if lifecycle.get("package_postinstall_helper_path_policy") != (
        "pre_exec_root_owned_0755_no_extended_acl_no_symlink_"
        "single_link_helper"
    ):
        fail("profile_pkg_postinstall_helper_path_policy_mismatch")

    identity = profile.get("os_identity", {})
    if identity.get("service_user") != SERVICE_USER:
        fail("profile_service_user_mismatch")
    if identity.get("group") != SERVICE_GROUP:
        fail("profile_service_group_mismatch")
    if identity.get("service_login") != "forbidden":
        fail("profile_service_login_not_forbidden")
    if identity.get("service_hidden") is not True:
        fail("profile_service_identity_not_hidden")
    if identity.get("service_password_record") != "literal_asterisk_lock":
        fail("profile_service_password_record_not_locked")
    if identity.get("service_authentication_authority") != "absent":
        fail("profile_service_authentication_authority_not_absent")
    if identity.get("service_shadow_hash_data") != "absent":
        fail("profile_service_shadow_hash_data_not_absent")
    if identity.get("service_administrator_group_membership") != "forbidden":
        fail("profile_service_admin_membership_not_forbidden")
    if (
        identity.get("service_explicit_supplementary_group_membership")
        != "forbidden"
    ):
        fail("profile_service_supplementary_membership_not_forbidden")
    if identity.get("service_group_group_membership") != (
        "exact_scratchbird_user_only"
    ):
        fail("profile_service_group_named_membership_not_exact")
    if identity.get("service_group_group_members") != (
        "exact_service_user_generated_uid_only"
    ):
        fail("profile_service_group_guid_membership_not_exact")
    if identity.get("service_group_nested_groups") != (
        "forbidden_and_verified_empty"
    ):
        fail("profile_service_group_nested_membership_not_empty")
    if identity.get("service_group_nested_in_other_local_group") != "forbidden":
        fail("profile_service_group_nesting_not_forbidden")
    if identity.get("service_implicit_baseline_groups") != (
        "macos_host_computed_directory_groups_are_not_installer_membership"
    ):
        fail("profile_service_implicit_baseline_policy_mismatch")
    if identity.get("service_resolved_effective_group_set") != (
        MACOS_SERVICE_PROCESS_GROUP_POLICY
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
    if service.get("launchd_init_groups") is not False:
        fail("profile_service_launchd_init_groups")
    if service.get("launchd_bootstrap_identity") != "root:wheel":
        fail("profile_service_launchd_bootstrap_identity")
    if service.get("launchd_definition_path_policy") != (
        "root_owned_0644_no_extended_acl_no_symlink_single_link_"
        "exact_fixed_selector"
    ):
        fail("profile_launchd_definition_path_policy_mismatch")
    if service.get("launchd_standard_log_root") != (
        "/var/log/scratchbird/launchd"
    ):
        fail("profile_service_launchd_log_root_mismatch")
    if service.get("launchd_standard_log_file_identity") != (
        "root:scratchbird:0640_no_extended_acl"
    ):
        fail("profile_service_launchd_log_file_identity_mismatch")
    if service.get("service_launcher") != MACOS_SERVICE_LAUNCHER:
        fail("profile_service_launcher_mismatch")
    if service.get("service_launcher_interface") != (
        "fixed_selector_only_no_forwarded_arguments"
    ):
        fail("profile_service_launcher_interface_mismatch")
    if service.get("service_launcher_path_policy") != (
        "root_owned_nonwritable_no_extended_acl_no_symlink_"
        "single_link_launcher"
    ):
        fail("profile_service_launcher_path_policy_mismatch")
    if service.get("final_product_identity") != "scratchbird:scratchbird":
        fail("profile_service_final_identity_mismatch")
    if service.get("final_supplementary_groups") != []:
        fail("profile_service_final_groups_not_empty")
    if service.get("service_runtime_log_root") != "/var/log/scratchbird/runtime":
        fail("profile_service_runtime_log_root_mismatch")
    upgrade = profile.get("upgrade_policy", {})
    if upgrade.get("loaded_legacy_launchd_job") != (
        "reject_in_preinstall_before_payload_replacement_and_recheck_in_"
        "postinstall_helper"
    ):
        fail("profile_loaded_legacy_launchd_upgrade_policy_mismatch")
    if upgrade.get("legacy_packaged_log_defaults") != (
        "exact_prior_packaged_line_only_preserve_all_other_configuration_lines"
    ):
        fail("profile_legacy_log_migration_policy_mismatch")
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
    directory_rows = {
        row.get("path"): row
        for row in profile.get("directories", [])
        if isinstance(row, dict)
    }
    expected_log_directories = {
        "/var/log/scratchbird": {
            "path": "/var/log/scratchbird",
            "owner": "root",
            "group": "scratchbird",
            "mode": "0750",
        },
        "/var/log/scratchbird/launchd": {
            "path": "/var/log/scratchbird/launchd",
            "owner": "root",
            "group": "scratchbird",
            "mode": "0750",
        },
        "/var/log/scratchbird/runtime": {
            "path": "/var/log/scratchbird/runtime",
            "owner": "scratchbird",
            "group": "scratchbird",
            "mode": "0750",
        },
    }
    for path, expected in expected_log_directories.items():
        if directory_rows.get(path) != expected:
            fail(f"profile_log_directory_authority_mismatch:{path}")
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
        if payload.get("UserName") != "root":
            fail(f"launchd_user_mismatch:{path.name}")
        if payload.get("GroupName") != "wheel":
            fail(f"launchd_group_mismatch:{path.name}")
        if payload.get("InitGroups") is not False:
            fail(f"launchd_init_groups_not_disabled:{path.name}")
        if payload.get("RunAtLoad") is not False:
            fail(f"launchd_run_at_load:{path.name}")
        if payload.get("KeepAlive") is not False:
            fail(f"launchd_keep_alive:{path.name}")
        if payload.get("Disabled") is not True:
            fail(f"launchd_not_disabled:{path.name}")
        expected_selector = {
            "com.scratchbird.sbsrv": "sbsrv",
            "com.scratchbird.sbmgr": "sbmgr",
        }[label]
        arguments = payload.get("ProgramArguments")
        if arguments != [MACOS_SERVICE_LAUNCHER, expected_selector]:
            fail(f"launchd_top_level_executable_mismatch:{path.name}")
        if "--create-if-missing" in arguments:
            fail(f"launchd_database_creation_forbidden:{path.name}")
        if any("SBgate" in str(value) or "SBParser" in str(value) for value in arguments):
            fail(f"launchd_child_process_service_forbidden:{path.name}")
        expected_log_prefix = "/var/log/scratchbird/launchd/"
        for key in ("StandardOutPath", "StandardErrorPath"):
            value = payload.get(key)
            if not isinstance(value, str) or not value.startswith(expected_log_prefix):
                fail(f"launchd_standard_log_path_invalid:{path.name}:{key}")
        rows.append(
            {
                "label": label,
                "arguments": arguments,
                "launchd_bootstrap_identity": "root:wheel",
                "final_product_identity": "scratchbird:scratchbird",
                "final_supplementary_groups": [],
                "init_groups": False,
            }
        )
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
        "PATH=/usr/bin:/bin:/usr/sbin:/sbin",
        "ensure_service_is_not_admin",
        "ensure_service_group_membership_is_exact",
        "ensure_service_has_no_explicit_supplementary_membership",
        '[ "$identity_mode" = fixture ] || return 0',
        '[ -d "$config_root" ] || return 0',
        "dsAttrType(Native|Standard):(AuthenticationAuthority|ShadowHashData)",
        '[ "$user_password" = \'*\' ]',
        "local_group_has_guid_member",
        "local_group_nests_group_guid",
        "GroupMembers",
        "NestedGroups",
        'membership_count=$((membership_count + 1))',
        'guid_membership_count=$((guid_membership_count + 1))',
        '[ "$membership_count" -eq 1 ]',
        '[ "$guid_membership_count" -eq 1 ]',
        'ensure_service_group_membership_is_exact "$user_generated_uid"',
        "launchd_host_computed_groups_cleared_before_scratchbird_product_exec",
        "make_regular_file 0640",
        "remove_extended_acl",
        'chmod -N "$path"',
        "validate_root_service_launcher_path",
        "validate_launchd_service_definitions",
        "root_owned_0644_no_extended_acl_no_symlink_single_link_exact_fixed_selector",
        "root_owned_nonwritable_no_extended_acl_no_symlink_single_link_launcher",
        "/var/log/scratchbird/launchd/SBsrv.out.log",
        "/var/log/scratchbird/launchd/SBmgr.err.log",
        "/var/log/scratchbird/runtime",
        "ensure_services_not_loaded",
        '/bin/launchctl print system',
        '/bin/launchctl print "system/$label"',
        '[ "$launchctl_status" -eq 113 ]',
        "migrate_legacy_packaged_log_defaults",
        "exact_prior_packaged_line_only_preserve_all_other_configuration_lines",
        'printf \'  "service_launcher": "/opt/ScratchBird/bin/SBlaunch"',
        'printf \'  "launchd_init_groups": false',
        "/Groups/admin",
        'dseditgroup -n . -o checkmember',
        'admin 2>/dev/null || true',
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
    if re.search(r"[|][|] return[ \t]*(?:\n|$)", text):
        fail("lifecycle_status_inheriting_bare_return")
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
        "/opt/ScratchBird/bin/SBsrv",
        "--create-if-missing",
        ".sbdb",
        'id -G "$SERVICE_USER"',
        "primary_scratchbird_plus_macos_implicit_gid_12_and_61_only",
        "host_computed_directory_groups_not_copied_into_service_process",
        ".sbrd",
        "security_principal_events",
        "local_password_auth",
        "--installer-user",
        "GROUP_MEMBERSHIP_REQUIRED",
        "admin 2>/dev/null) ||",
        'dscl . -read "/Users/$SERVICE_USER" AuthenticationAuthority',
        'dscl . -read "/Users/$SERVICE_USER" ShadowHashData',
    ):
        if forbidden in text:
            fail(f"lifecycle_forbidden_fragment:{forbidden}")
    for forbidden_port in (3050, 3090, 3392):
        if str(forbidden_port) in text:
            fail(f"lifecycle_forbidden_native_port:{forbidden_port}")


def validate_launcher_static(launcher: Path) -> dict[str, Any]:
    if not launcher.is_file():
        fail("service_launcher_source_missing")
    text = launcher.read_text(encoding="utf-8")
    for fragment in (
        'strcmp(argv[1], "sbsrv")',
        'strcmp(argv[1], "sbmgr")',
        'strcmp(argv[1], "credential-probe")',
        '"/opt/ScratchBird/bin/SBsrv"',
        '"/opt/ScratchBird/bin/SBmgr"',
        '"--verify-running-service-identity"',
        "setgroups(0, NULL)",
        "setgid(runtime_gid)",
        "setuid(runtime_uid)",
        'ScratchBirdKernelGetGroups(int size, gid_t groups[])',
        'validate_executable_target(target, runtime_gid)',
        "metadata.st_nlink",
        "close_inherited_descriptors()",
        "execve(target, selected_argv, clean_environment)",
        '"PATH=/usr/bin:/bin:/usr/sbin:/sbin"',
        'deny("SB_MACOS_LAUNCHER.SELECTOR_DENIED")',
    ):
        if fragment not in text:
            fail(f"service_launcher_contract_missing:{fragment}")
    for forbidden in (
        "system(",
        "popen(",
        "execlp(",
        "execvp(",
        "posix_spawn",
        "fork(",
        "--target",
    ):
        if forbidden in text:
            fail(f"service_launcher_forbidden_fragment:{forbidden}")
    if text.count("validate_executable_target(target, runtime_gid)") != 2:
        fail("service_launcher_target_revalidation_missing")
    return {
        "fixed_selectors": ["sbsrv", "sbmgr", "credential-probe"],
        "supplementary_groups_cleared": True,
        "final_identity": "scratchbird:scratchbird",
        "arbitrary_target_or_arguments": "forbidden",
    }


def validate_pkg_scripts(asset_root: Path) -> dict[str, Any]:
    preinstall = asset_root / "pkg-scripts" / "preinstall.in"
    postinstall = asset_root / "pkg-scripts" / "postinstall.in"
    if not preinstall.is_file() or not postinstall.is_file():
        fail("pkg_lifecycle_template_missing")
    preinstall_text = preinstall.read_text(encoding="utf-8")
    for fragment in (
        "BOOTSTRAP.SERVICE_STATE_INVALID",
        "PATH=/usr/bin:/bin:/usr/sbin:/sbin",
        "validate_existing_directory /opt true",
        "validate_existing_file /opt/ScratchBird/bin/SBlaunch 755",
        "/Library/LaunchDaemons/com.scratchbird.sbsrv.plist 644",
        "/bin/launchctl print system",
        '/bin/launchctl print "system/$label"',
        'if [ "$launchctl_status" -ne 113 ]',
        "com.scratchbird.sbsrv com.scratchbird.sbmgr",
    ):
        if fragment not in preinstall_text:
            fail(f"pkg_preinstall_contract_missing:{fragment}")
    text = postinstall.read_text(encoding="utf-8")
    required = (
        "/opt/ScratchBird/libexec/scratchbird-macos-system-install",
        "PATH=/usr/bin:/bin:/usr/sbin:/sbin",
        "require_no_extended_acl",
        "/opt/ScratchBird/libexec",
        "root:wheel:755",
        "'%l'",
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
        if forbidden in preinstall_text:
            fail(f"pkg_preinstall_forbidden_fragment:{forbidden}")
    return {
        "preinstall_template": "present",
        "postinstall_template": "present",
        "loaded_legacy_launchd_job": "rejected_before_payload_replacement",
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
    portable_bin = portable_root / "opt" / "ScratchBird" / "bin"
    portable_bin.mkdir(parents=True)
    portable_launcher = portable_bin / "SBlaunch"
    portable_launcher.write_bytes(b"fixture-macos-service-launcher\n")
    portable_launcher.chmod(0o755)
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
    launchd_services = launchd_manifest.get("services")
    if not isinstance(launchd_services, list) or not launchd_services:
        fail("builder_launchd_manifest_services_missing")
    if any(row.get("init_groups") is not False for row in launchd_services):
        fail("builder_launchd_manifest_init_groups_not_disabled")
    if any(row.get("launchd_bootstrap_user") != "root" for row in launchd_services):
        fail("builder_launchd_manifest_bootstrap_user_mismatch")
    if any(row.get("final_user") != SERVICE_USER for row in launchd_services):
        fail("builder_launchd_manifest_final_user_mismatch")
    if any(row.get("final_supplementary_groups") != [] for row in launchd_services):
        fail("builder_launchd_manifest_final_groups_not_empty")

    staged_helper = (
        system_root
        / "opt"
        / "ScratchBird"
        / "libexec"
        / "scratchbird-macos-system-install"
    )
    if not staged_helper.is_file() or not os.access(staged_helper, os.X_OK):
        fail("builder_lifecycle_helper_not_staged")
    staged_launcher = system_root / "opt" / "ScratchBird" / "bin" / "SBlaunch"
    if not staged_launcher.is_file() or not os.access(staged_launcher, os.X_OK):
        fail("builder_service_launcher_not_staged")
    postinstall = scripts_root / "postinstall"
    preinstall = scripts_root / "preinstall"
    if (
        not preinstall.is_file()
        or not os.access(preinstall, os.X_OK)
        or not postinstall.is_file()
        or not os.access(postinstall, os.X_OK)
    ):
        fail("builder_pkg_lifecycle_scripts_not_materialized")
    preinstall_text = preinstall.read_text(encoding="utf-8")
    if "@SCRATCHBIRD_VERSION@" in preinstall_text:
        fail("builder_pkg_preinstall_unexpected_version_token")
    if '/bin/launchctl print "system/$label"' not in preinstall_text:
        fail("builder_pkg_preinstall_loaded_job_guard_missing")
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
    if evidence.get("os_identity", {}).get("group_membership_policy") != (
        "exact_scratchbird_name_and_generated_uid_only_no_nested_groups"
    ):
        fail("builder_system_evidence_group_membership_policy_mismatch")
    if evidence.get("os_identity", {}).get("resolved_effective_group_policy") != (
        MACOS_SERVICE_PROCESS_GROUP_POLICY
    ):
        fail("builder_system_evidence_process_group_policy_mismatch")
    if evidence.get("service", {}).get("launchd_init_groups") is not False:
        fail("builder_system_evidence_launchd_init_groups_not_disabled")
    if evidence.get("service", {}).get("launchd_bootstrap_identity") != "root:wheel":
        fail("builder_system_evidence_bootstrap_identity_mismatch")
    if evidence.get("service", {}).get("launchd_definition_path_policy") != (
        "root_owned_0644_no_extended_acl_no_symlink_single_link_"
        "exact_fixed_selector"
    ):
        fail("builder_system_evidence_launchd_definition_path_policy_mismatch")
    if evidence.get("service", {}).get("launchd_standard_log_root") != (
        "/var/log/scratchbird/launchd"
    ):
        fail("builder_system_evidence_launchd_log_root_mismatch")
    if evidence.get("service", {}).get("launchd_standard_log_file_identity") != (
        "root:scratchbird:0640_no_extended_acl"
    ):
        fail("builder_system_evidence_launchd_log_file_identity_mismatch")
    if evidence.get("service", {}).get("service_launcher_path_policy") != (
        "root_owned_nonwritable_no_extended_acl_no_symlink_"
        "single_link_launcher"
    ):
        fail("builder_system_evidence_launcher_path_policy_mismatch")
    if evidence.get("service", {}).get("final_product_identity") != (
        "scratchbird:scratchbird"
    ):
        fail("builder_system_evidence_final_identity_mismatch")
    if evidence.get("service", {}).get("final_supplementary_groups") != []:
        fail("builder_system_evidence_final_groups_not_empty")
    if evidence.get("service", {}).get("service_runtime_log_root") != (
        "/var/log/scratchbird/runtime"
    ):
        fail("builder_system_evidence_runtime_log_root_mismatch")
    if evidence.get("service", {}).get(
        "loaded_legacy_launchd_job_upgrade_policy"
    ) != "reject_before_payload_replacement_and_recheck_postinstall":
        fail("builder_system_evidence_loaded_job_upgrade_policy_mismatch")
    if evidence.get("service", {}).get(
        "package_preinstall_existing_topology_policy"
    ) != "reject_unsafe_existing_root_helper_launcher_and_plist_paths_before_payload":
        fail("builder_system_evidence_preinstall_topology_policy_mismatch")
    if evidence.get("service", {}).get(
        "package_postinstall_helper_path_policy"
    ) != "pre_exec_root_owned_0755_no_extended_acl_no_symlink_single_link_helper":
        fail("builder_system_evidence_postinstall_helper_path_policy_mismatch")
    if evidence.get("service", {}).get(
        "legacy_packaged_log_default_migration_policy"
    ) != "exact_prior_packaged_line_only_preserve_all_other_configuration_lines":
        fail("builder_system_evidence_legacy_log_migration_policy_mismatch")

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
        if payload.get("UserName") != "root":
            fail(f"builder_launchd_user_mismatch:{path.name}")
        if payload.get("GroupName") != "wheel":
            fail(f"builder_launchd_group_mismatch:{path.name}")
        if payload.get("InitGroups") is not False:
            fail(f"builder_launchd_init_groups_not_disabled:{path.name}")
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
        "service_launcher_staged": True,
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
    for name in (
        "SBsrv.out.log",
        "SBsrv.err.log",
        "SBmgr.out.log",
        "SBmgr.err.log",
    ):
        log_path = (
            fixture_root
            / "var"
            / "log"
            / "scratchbird"
            / "launchd"
            / name
        )
        if not log_path.is_file() or log_path.is_symlink():
            fail(f"fixture_launchd_log_missing_or_invalid:{name}")
        require_mode(log_path, 0o640)
    for name in ("SBsrv.log", "SBmgr.log"):
        if (fixture_root / "var" / "log" / "scratchbird" / "runtime" / name).exists():
            fail(f"fixture_inactive_application_log_created:{name}")
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
    expected_fixture_uid = "fixture-scratchbird-user-generated-uid"
    if group != {
        "name": SERVICE_GROUP,
        "kind": "local_group",
        "group_membership": [SERVICE_USER],
        "group_members": [expected_fixture_uid],
        "nested_groups": [],
    }:
        fail("fixture_group_identity_mismatch")
    if (
        service.get("name") != SERVICE_USER
        or service.get("kind") != "non_login_service"
        or service.get("hidden") is not True
        or service.get("password_record") != "literal_asterisk_lock"
        or service.get("authentication_authority_present") is not False
        or service.get("shadow_hash_data_present") is not False
        or service.get("administrator_group_membership") is not False
        or service.get("primary_group") != SERVICE_GROUP
        or service.get("generated_uid") != expected_fixture_uid
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
        "service_password_record_locked": True,
        "service_authentication_authority_present": False,
        "service_shadow_hash_data_present": False,
        "service_administrator_group_membership": False,
        "service_authority_scope": (
            "filesystem_directory_and_process_execution_only_"
            "no_database_or_security_authority"
        ),
        "service_group_membership_policy": (
            "exact_scratchbird_name_and_generated_uid_only_no_nested_groups"
        ),
        "resolved_effective_group_policy": (
            MACOS_SERVICE_PROCESS_GROUP_POLICY
        ),
        "launchd_init_groups": False,
        "launchd_bootstrap_identity": "root:wheel",
        "launchd_definition_path_policy": (
            "root_owned_0644_no_extended_acl_no_symlink_single_link_"
            "exact_fixed_selector"
        ),
        "package_postinstall_helper_path_policy": (
            "pre_exec_root_owned_0755_no_extended_acl_no_symlink_"
            "single_link_helper"
        ),
        "service_launcher": "/opt/ScratchBird/bin/SBlaunch",
        "service_launcher_path_policy": (
            "root_owned_nonwritable_no_extended_acl_no_symlink_"
            "single_link_launcher"
        ),
        "final_product_identity": "scratchbird:scratchbird",
        "final_supplementary_groups_empty": True,
        "human_service_group_membership_mutated": False,
        "create_time_os_authorization": "root_only",
        "service_enablement_default": "disabled",
        "service_activity_default": "not_started",
        "launchd_load_performed": False,
        "legacy_packaged_log_default_migration_policy": (
            "exact_prior_packaged_line_only_preserve_all_other_"
            "configuration_lines"
        ),
        "legacy_packaged_log_default_migrations": 0,
        "native_default_port": NATIVE_PORT,
        "database_files_created": False,
        "security_sidecars_created": False,
    }
    for key, expected in expected_state.items():
        if state.get(key) != expected:
            fail(f"fixture_state_mismatch:{key}")

    config_root = fixture_root / "Library" / "Application Support" / "ScratchBird"
    server_config = config_root / "SBsrv.conf"
    server_text = server_config.read_text(encoding="utf-8").replace(
        "log_file = /var/log/scratchbird/runtime/SBsrv.log",
        "log_file = /var/log/scratchbird/SBsrv.log",
        1,
    )
    server_config.write_text(
        server_text + "operator_override = preserved\n", encoding="utf-8"
    )
    manager_config = config_root / "SBmgr.conf"
    manager_config.write_text(
        manager_config.read_text(encoding="utf-8").replace(
            "manager.log.path = /var/log/scratchbird/runtime/SBmgr.log",
            "manager.log.path = /var/log/scratchbird/SBmgr.log",
            1,
        ),
        encoding="utf-8",
    )
    expected_server_after_migration = server_config.read_bytes().replace(
        b"log_file = /var/log/scratchbird/SBsrv.log",
        b"log_file = /var/log/scratchbird/runtime/SBsrv.log",
        1,
    )
    expected_manager_after_migration = manager_config.read_bytes().replace(
        b"manager.log.path = /var/log/scratchbird/SBmgr.log",
        b"manager.log.path = /var/log/scratchbird/runtime/SBmgr.log",
        1,
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
    if "log_file = /var/log/scratchbird/runtime/SBsrv.log" not in (
        server_config.read_text(encoding="utf-8")
    ):
        fail("fixture_upgrade_server_log_default_not_migrated")
    if "manager.log.path = /var/log/scratchbird/runtime/SBmgr.log" not in (
        manager_config.read_text(encoding="utf-8")
    ):
        fail("fixture_upgrade_manager_log_default_not_migrated")
    if server_config.read_bytes() != expected_server_after_migration:
        fail("fixture_upgrade_server_config_bytes_not_preserved")
    if manager_config.read_bytes() != expected_manager_after_migration:
        fail("fixture_upgrade_manager_config_bytes_not_preserved")
    repeated_state = json.loads(state_path.read_text(encoding="utf-8"))
    if repeated_state.get("legacy_packaged_log_default_migrations") != 2:
        fail("fixture_upgrade_log_default_migration_count_mismatch")
    idempotent_config_bytes = {
        path: path.read_bytes() for path in (server_config, manager_config)
    }
    idempotent = run(lifecycle_command(helper, fixture_root))
    if idempotent.returncode != 0:
        fail(
            f"fixture_migration_idempotence_failed:{idempotent.returncode}:"
            f"{idempotent.stderr.strip()}"
        )
    if any(path.read_bytes() != body for path, body in idempotent_config_bytes.items()):
        fail("fixture_migration_idempotence_changed_config")
    manager_config.write_text(
        manager_config.read_text(encoding="utf-8").replace(
            "manager.log.path = /var/log/scratchbird/runtime/SBmgr.log",
            "manager.log.path = /var/log/scratchbird/SBmgr.log",
            1,
        ),
        encoding="utf-8",
    )
    partial_server_before = server_config.read_bytes()
    partial_resume = run(lifecycle_command(helper, fixture_root))
    if partial_resume.returncode != 0:
        fail(
            f"fixture_partial_migration_resume_failed:{partial_resume.returncode}:"
            f"{partial_resume.stderr.strip()}"
        )
    if server_config.read_bytes() != partial_server_before:
        fail("fixture_partial_migration_resume_changed_server_config")
    if "manager.log.path = /var/log/scratchbird/runtime/SBmgr.log" not in (
        manager_config.read_text(encoding="utf-8")
    ):
        fail("fixture_partial_migration_resume_manager_not_migrated")
    partial_state = json.loads(state_path.read_text(encoding="utf-8"))
    if partial_state.get("legacy_packaged_log_default_migrations") != 1:
        fail("fixture_partial_migration_resume_count_mismatch")

    ambiguous_root = work_root / "migration-ambiguous"
    shutil.copytree(fixture_root, ambiguous_root)
    ambiguous_config = (
        ambiguous_root / "Library" / "Application Support" / "ScratchBird"
    )
    ambiguous_server = ambiguous_config / "SBsrv.conf"
    ambiguous_manager = ambiguous_config / "SBmgr.conf"
    ambiguous_server.write_text(
        ambiguous_server.read_text(encoding="utf-8").replace(
            "log_file = /var/log/scratchbird/runtime/SBsrv.log",
            "log_file = /var/log/scratchbird/SBsrv.log",
            1,
        ),
        encoding="utf-8",
    )
    ambiguous_manager.write_text(
        ambiguous_manager.read_text(encoding="utf-8")
        + "manager.log.path = /var/log/scratchbird/SBmgr.log\n",
        encoding="utf-8",
    )
    ambiguous_before = {
        path: path.read_bytes() for path in (ambiguous_server, ambiguous_manager)
    }
    ambiguous = run(lifecycle_command(helper, ambiguous_root))
    if (
        ambiguous.returncode == 0
        or ambiguous.stderr != "BOOTSTRAP.INSTALL_DEFAULTS_INVALID\n"
    ):
        fail("fixture_ambiguous_log_migration_not_refused_code_only")
    if any(path.read_bytes() != body for path, body in ambiguous_before.items()):
        fail("fixture_ambiguous_log_migration_partially_changed_config")

    newline_root = work_root / "migration-no-final-newline"
    shutil.copytree(fixture_root, newline_root)
    newline_server = (
        newline_root
        / "Library"
        / "Application Support"
        / "ScratchBird"
        / "SBsrv.conf"
    )
    newline_body = newline_server.read_bytes().replace(
        b"log_file = /var/log/scratchbird/runtime/SBsrv.log",
        b"log_file = /var/log/scratchbird/SBsrv.log",
        1,
    ).rstrip(b"\n")
    newline_server.write_bytes(newline_body)
    newline = run(lifecycle_command(helper, newline_root))
    if newline.returncode == 0 or newline.stderr != (
        "BOOTSTRAP.INSTALL_DEFAULTS_INVALID\n"
    ):
        fail("fixture_noncanonical_line_topology_not_refused_code_only")
    if newline_server.read_bytes() != newline_body:
        fail("fixture_noncanonical_line_topology_changed_config")

    hardlink_root = work_root / "migration-hardlink"
    shutil.copytree(fixture_root, hardlink_root)
    hardlink_server = (
        hardlink_root
        / "Library"
        / "Application Support"
        / "ScratchBird"
        / "SBsrv.conf"
    )
    external_server = work_root / "migration-hardlink-external.conf"
    external_server.write_bytes(hardlink_server.read_bytes())
    external_server.chmod(0o600)
    hardlink_server.unlink()
    os.link(external_server, hardlink_server)
    external_before = (external_server.read_bytes(), stat.S_IMODE(external_server.stat().st_mode))
    hardlink = run(lifecycle_command(helper, hardlink_root))
    if hardlink.returncode == 0 or hardlink.stderr != (
        "BOOTSTRAP.DIRECTORY_PERMISSION_INVALID\n"
    ):
        fail("fixture_hardlinked_config_not_refused_code_only")
    external_after = (external_server.read_bytes(), stat.S_IMODE(external_server.stat().st_mode))
    if external_after != external_before:
        fail("fixture_hardlinked_external_inode_changed")

    custom_root = work_root / "migration-custom-root-log"
    shutil.copytree(fixture_root, custom_root)
    custom_server = (
        custom_root
        / "Library"
        / "Application Support"
        / "ScratchBird"
        / "SBsrv.conf"
    )
    custom_body = custom_server.read_text(encoding="utf-8").replace(
        "log_file = /var/log/scratchbird/runtime/SBsrv.log",
        'LOG_FILE = "/var/log/scratchbird/runtime/../custom-SBsrv.log"',
        1,
    )
    custom_server.write_text(custom_body, encoding="utf-8")
    custom = run(lifecycle_command(helper, custom_root))
    if custom.returncode == 0 or custom.stderr != (
        "BOOTSTRAP.INSTALL_DEFAULTS_INVALID\n"
    ):
        fail("fixture_custom_root_log_assignment_not_refused_code_only")
    if custom_server.read_text(encoding="utf-8") != custom_body:
        fail("fixture_custom_root_log_refusal_changed_config")

    manager_escape_root = work_root / "migration-manager-root-log-escape"
    shutil.copytree(fixture_root, manager_escape_root)
    manager_escape = (
        manager_escape_root
        / "Library"
        / "Application Support"
        / "ScratchBird"
        / "SBmgr.conf"
    )
    manager_escape_body = manager_escape.read_text(encoding="utf-8").replace(
        "manager.log.path = /var/log/scratchbird/runtime/SBmgr.log",
        "manager.log.path = /var/log/scratchbird/runtime/../custom-SBmgr.log",
        1,
    )
    manager_escape.write_text(manager_escape_body, encoding="utf-8")
    manager_escape_result = run(
        lifecycle_command(helper, manager_escape_root)
    )
    if manager_escape_result.returncode == 0 or manager_escape_result.stderr != (
        "BOOTSTRAP.INSTALL_DEFAULTS_INVALID\n"
    ):
        fail("fixture_manager_root_log_escape_not_refused_code_only")
    if manager_escape.read_text(encoding="utf-8") != manager_escape_body:
        fail("fixture_manager_root_log_escape_changed_config")

    private_alias_root = work_root / "migration-private-var-alias"
    shutil.copytree(fixture_root, private_alias_root)
    private_alias_server = (
        private_alias_root
        / "Library"
        / "Application Support"
        / "ScratchBird"
        / "SBsrv.conf"
    )
    private_alias_body = private_alias_server.read_text(encoding="utf-8").replace(
        "log_file = /var/log/scratchbird/runtime/SBsrv.log",
        'LOG_FILE = "/private/var/log/scratchbird/custom-SBsrv.log"',
        1,
    )
    private_alias_server.write_text(private_alias_body, encoding="utf-8")
    private_alias_result = run(lifecycle_command(helper, private_alias_root))
    if private_alias_result.returncode == 0 or private_alias_result.stderr != (
        "BOOTSTRAP.INSTALL_DEFAULTS_INVALID\n"
    ):
        fail("fixture_private_var_log_alias_not_refused_code_only")
    if private_alias_server.read_text(encoding="utf-8") != private_alias_body:
        fail("fixture_private_var_log_alias_changed_config")

    relative_alias_root = work_root / "migration-relative-root-log-alias"
    shutil.copytree(fixture_root, relative_alias_root)
    relative_alias_manager = (
        relative_alias_root
        / "Library"
        / "Application Support"
        / "ScratchBird"
        / "SBmgr.conf"
    )
    relative_alias_body = relative_alias_manager.read_text(
        encoding="utf-8"
    ).replace(
        "manager.log.path = /var/log/scratchbird/runtime/SBmgr.log",
        "manager.log.path = ../../var/log/scratchbird/custom-SBmgr.log",
        1,
    )
    relative_alias_manager.write_text(relative_alias_body, encoding="utf-8")
    relative_alias_result = run(lifecycle_command(helper, relative_alias_root))
    if relative_alias_result.returncode == 0 or relative_alias_result.stderr != (
        "BOOTSTRAP.INSTALL_DEFAULTS_INVALID\n"
    ):
        fail("fixture_relative_root_log_alias_not_refused_code_only")
    if relative_alias_manager.read_text(encoding="utf-8") != relative_alias_body:
        fail("fixture_relative_root_log_alias_changed_config")
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
        "legacy_packaged_log_default_migration": "passed",
        "partial_migration_resume": "passed",
        "legacy_log_migration_ambiguity_refusal": "passed",
        "legacy_log_migration_line_topology_refusal": "passed",
        "configuration_hardlink_refusal": "passed",
        "custom_root_log_assignment_refusal": "passed",
        "manager_root_log_escape_refusal": "passed",
        "private_var_log_alias_refusal": "passed",
        "relative_root_log_alias_refusal": "passed",
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
        "service_launcher": validate_launcher_static(
            asset_root / "scratchbird-macos-service-launcher.c"
        ),
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
