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


def has_listener_profile_section(text: str) -> bool:
    return any(
        line.strip().startswith("[server.listener.profile.")
        and line.strip().endswith("]")
        for line in text.splitlines()
    )


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
    if service.get("service_launcher") != MACOS_SERVICE_LAUNCHER:
        fail("profile_service_launcher_mismatch")
    if service.get("service_launcher_interface") != (
        "fixed_selector_only_no_forwarded_arguments"
    ):
        fail("profile_service_launcher_interface_mismatch")
    if service.get("final_product_identity") != "scratchbird:scratchbird":
        fail("profile_service_final_identity_mismatch")
    if service.get("final_supplementary_groups") != []:
        fail("profile_service_final_groups_not_empty")
    if service.get("launchd_standard_log_root") != "/var/log/scratchbird/launchd":
        fail("profile_service_launchd_log_root_mismatch")
    if service.get("service_runtime_log_root") != "/var/log/scratchbird/runtime":
        fail("profile_service_runtime_log_root_mismatch")
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
        "BOOTSTRAP.MACOS_IDENTITY_VALIDATION_STAGE=",
        "BOOTSTRAP.MACOS_INSTALL_VALIDATION_STAGE=",
        "install_validation_stage=argument_validation",
        "log_system_installer_diagnostic()",
        "report_unclassified_install_failure()",
        "installer_failure_reported=false",
        "trap 'report_unclassified_install_failure \"$?\"' 0",
        "/usr/bin/logger -t scratchbird-installer",
        '"scratchbird-installer stage=$install_validation_stage code=$diagnostic_code"',
        "BOOTSTRAP.MACOS_INSTALL_STAGE_UNCLASSIFIED_FAILURE",
        "service_state_validation",
        "service_identity_validation",
        "directory_configuration",
        "default_configuration_content",
        "default_configuration_symlink_validation",
        "default_configuration_migration",
        "default_configuration_permissions",
        "root_service_authority_validation",
        "root_opt_directory_validation",
        "root_scratchbird_directory_normalization",
        "root_bin_directory_normalization",
        "root_sblaunch_normalization",
        "root_sbsrv_normalization",
        "root_sbmgr_normalization",
        "launchd_directory_validation",
        "launchd_sbsrv_definition_validation",
        "launchd_sbmgr_definition_validation",
        "install_evidence_write",
        "pre_remove_configuration_preservation",
        "identity_record_validation",
        "service_supplementary_group_validation",
        "service_admin_membership_validation",
        "ensure_service_is_not_admin",
        "ensure_service_group_membership_is_exact",
        "ensure_service_has_no_explicit_supplementary_membership",
        "local_group_has_guid_member",
        "local_group_nests_group_guid",
        "GroupMembers",
        "NestedGroups",
        "/Groups/admin",
        'LC_ALL=C dseditgroup -n . -o checkmember',
        "effective_admin_membership_status=$?",
        '"no $SERVICE_USER is NOT a member of admin"',
        "67) ;;",
        "ensure_services_not_loaded",
        "validate_root_service_launcher_path",
        "validate_launchd_service_definitions",
        "/opt/ScratchBird/bin/SBlaunch",
        "/var/log/scratchbird/launchd/SBsrv.out.log",
        "/var/log/scratchbird/runtime",
        "migrate_legacy_packaged_log_defaults",
        "launchd_host_computed_groups_cleared_before_scratchbird_product_exec",
        "dscl . -create",
        "/usr/bin/uuidgen",
        'GeneratedUID "$generated_uid"',
        "created_service_user_generated_uid=$generated_uid",
        '[ "$user_generated_uid" != "$created_service_user_generated_uid" ]',
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
        "--create-if-missing",
        'id -G "$SERVICE_USER"',
        "primary_scratchbird_plus_macos_implicit_gid_12_and_61_only",
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
        "require_no_additional_group_authority",
        "if (count != 1)",
        "groups[index] != effective_group",
        'ScratchBirdKernelGetGroups(int size, gid_t groups[])',
        '__asm("_getgroups")',
        "validate_executable_target(target, runtime_gid)",
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
        '__asm("_getgroups$DARWIN_EXTSN")',
    ):
        if forbidden in text:
            fail(f"service_launcher_forbidden_fragment:{forbidden}")
    if text.count("validate_executable_target(target, runtime_gid)") != 2:
        fail("service_launcher_target_revalidation_missing")
    return {
        "fixed_selectors": ["sbsrv", "sbmgr", "credential-probe"],
        "supplementary_groups_cleared": True,
        "group_access_list_policy": "exactly_one_effective_gid",
        "final_identity": "scratchbird:scratchbird",
        "arbitrary_target_or_arguments": "forbidden",
    }


def run_checkmember_negative_status_smoke(
    helper: Path,
    work_root: Path,
) -> dict[str, str]:
    """Execute the actual admin-membership helper with deterministic tools.

    macOS returns status 67 for a negative ``dseditgroup checkmember``
    predicate.  The fixture keeps this system-only branch executable on every
    POSIX release runner, while taking the shell function directly from the
    packaged helper rather than maintaining a second implementation.
    """

    helper_text = helper.read_text(encoding="utf-8")
    function_start = helper_text.find("ensure_service_is_not_admin() {")
    next_function = helper_text.find(
        "\ncreate_service_user() {",
        function_start,
    )
    if function_start < 0 or next_function < 0:
        fail("checkmember_helper_function_missing")
    function_text = helper_text[function_start:next_function].rstrip()

    fixture_root = work_root / "checkmember-negative-status"
    fixture_bin = fixture_root / "bin"
    fixture_bin.mkdir(parents=True)
    dscl = fixture_bin / "dscl"
    dscl.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
    dscl.chmod(0o755)
    dseditgroup = fixture_bin / "dseditgroup"
    dseditgroup.write_text(
        "#!/bin/sh\n"
        "printf '%s\\n' \"${SB_CHECKMEMBER_OUTPUT:-}\"\n"
        "exit \"${SB_CHECKMEMBER_STATUS:?}\"\n",
        encoding="utf-8",
    )
    dseditgroup.chmod(0o755)

    harness = fixture_root / "checkmember-harness.sh"
    harness.write_text(
        "#!/bin/sh\n"
        "set -eu\n"
        "SERVICE_USER=scratchbird\n"
        "fail() {\n"
        "    printf '%s\\n' \"$1\" >&2\n"
        "    exit 1\n"
        "}\n"
        "local_group_has_member() {\n"
        "    return 1\n"
        "}\n\n"
        f"{function_text}\n"
        "ensure_service_is_not_admin 00000000-0000-0000-0000-000000000000\n",
        encoding="utf-8",
    )
    harness.chmod(0o755)

    def run_case(
        name: str,
        output: str,
        status: int,
        expected_success: bool,
    ) -> None:
        environment = os.environ.copy()
        environment["PATH"] = f"{fixture_bin}{os.pathsep}{environment['PATH']}"
        environment["SB_CHECKMEMBER_OUTPUT"] = output
        environment["SB_CHECKMEMBER_STATUS"] = str(status)
        result = subprocess.run(
            ["/bin/sh", str(harness)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            env=environment,
        )
        if expected_success:
            if result.returncode != 0:
                fail(
                    "checkmember_negative_status_rejected:"
                    f"{name}:{result.returncode}:{result.stderr.strip()}"
                )
            if result.stderr:
                fail(f"checkmember_negative_status_diagnostic:{name}")
            return
        if result.returncode == 0:
            fail(f"checkmember_invalid_response_accepted:{name}")
        if result.stderr != "BOOTSTRAP.GROUP_INPUT_INVALID\n":
            fail(f"checkmember_invalid_response_diagnostic:{name}")

    negative = "no scratchbird is NOT a member of admin"
    run_case("negative_status_67", negative, 67, True)
    run_case("positive_membership", "yes scratchbird is a member of admin", 0, False)
    run_case("negative_wrong_status", negative, 0, False)
    run_case("malformed_negative", "no scratchbird", 67, False)
    run_case("lookup_failure", "Group not found", 64, False)
    return {
        "negative_status_67": "accepted",
        "positive_membership": "rejected",
        "malformed_and_lookup_failures": "rejected",
    }


def run_service_user_creation_smoke(
    helper: Path,
    work_root: Path,
) -> dict[str, str]:
    """Exercise the packaged raw-dscl creation sequence with a fake directory.

    The system helper must write a GeneratedUID while it builds a fresh hidden
    service account, preserve that generated value for its immediate readback
    validation, and remove the incomplete account if that attribute write
    fails.  This executes the exact helper function so the hosted package
    cannot regress to an underspecified raw ``dscl`` record.
    """

    helper_text = helper.read_text(encoding="utf-8")
    function_start = helper_text.find("create_service_user() {")
    next_function = helper_text.find(
        "\nensure_service_identity() {",
        function_start,
    )
    if function_start < 0 or next_function < 0:
        fail("service_user_creation_helper_function_missing")
    function_text = helper_text[function_start:next_function].rstrip()
    if function_text.count("/usr/bin/uuidgen") != 1:
        fail("service_user_creation_uuidgen_contract_missing")

    fixture_root = work_root / "service-user-creation"
    fixture_bin = fixture_root / "bin"
    fixture_bin.mkdir(parents=True)
    generated_uid = "01234567-89ab-cdef-0123-456789abcdef"
    uuidgen = fixture_bin / "uuidgen"
    uuidgen.write_text(
        "#!/bin/sh\n"
        f"printf '%s\\n' '{generated_uid}'\n",
        encoding="utf-8",
    )
    uuidgen.chmod(0o755)
    function_text = function_text.replace("/usr/bin/uuidgen", str(uuidgen))
    dscl = fixture_bin / "dscl"
    dscl.write_text(
        "#!/bin/sh\n"
        "set -eu\n"
        "printf '%s|' \"$@\" >> \"${SB_DSCL_LOG:?}\"\n"
        "printf '\\n' >> \"${SB_DSCL_LOG:?}\"\n"
        "if [ \"$1\" = . ] && [ \"$2\" = -list ] && "
        "[ \"${3:-}\" = /Users ] && [ \"${4:-}\" = UniqueID ]; then\n"
        "    printf '%s\\n' 'root 0' 'daemon 1'\n"
        "    exit 0\n"
        "fi\n"
        "if [ \"$1\" = . ] && [ \"$2\" = -create ] && "
        "[ \"${3:-}\" = /Users/scratchbird ] && "
        "[ \"${4:-}\" = GeneratedUID ] && "
        "[ \"${SB_FAIL_GENERATED_UID_CREATE:-0}\" = 1 ]; then\n"
        "    exit 1\n"
        "fi\n"
        "exit 0\n",
        encoding="utf-8",
    )
    dscl.chmod(0o755)

    harness = fixture_root / "service-user-creation-harness.sh"
    harness.write_text(
        "#!/bin/sh\n"
        "set -eu\n"
        "SERVICE_USER=scratchbird\n"
        "SERVICE_UID_MIN=501\n"
        "SERVICE_UID_MAX_EXCLUSIVE=60000\n"
        "SERVICE_HOME=/var/lib/scratchbird\n"
        "NON_LOGIN_SHELL=/usr/bin/false\n"
        "fail() {\n"
        "    printf '%s\\n' \"$1\" >&2\n"
        "    exit 1\n"
        "}\n\n"
        f"{function_text}\n"
        "create_service_user 700\n"
        "printf 'generated_uid=%s\\n' \"$created_service_user_generated_uid\"\n",
        encoding="utf-8",
    )
    harness.chmod(0o755)

    def run_case(name: str, fail_generated_uid_create: bool) -> list[str]:
        log_path = fixture_root / f"{name}.dscl.log"
        environment = os.environ.copy()
        environment["PATH"] = f"{fixture_bin}{os.pathsep}{environment['PATH']}"
        environment["SB_DSCL_LOG"] = str(log_path)
        environment["SB_FAIL_GENERATED_UID_CREATE"] = (
            "1" if fail_generated_uid_create else "0"
        )
        result = subprocess.run(
            ["/bin/sh", str(harness)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            env=environment,
        )
        commands = log_path.read_text(encoding="utf-8").splitlines()
        if fail_generated_uid_create:
            if result.returncode == 0:
                fail("service_user_generated_uid_create_accepted")
            if result.stdout:
                fail("service_user_generated_uid_failure_stdout")
            if result.stderr != "BOOTSTRAP.GROUP_CREATE_FAILED\n":
                fail("service_user_generated_uid_failure_diagnostic")
            if not commands or commands[-1] != ".|-delete|/Users/scratchbird|":
                fail("service_user_generated_uid_failure_cleanup_missing")
            generated_uid_index = next(
                (
                    index
                    for index, command in enumerate(commands)
                    if command.startswith(
                        ".|-create|/Users/scratchbird|GeneratedUID|"
                    )
                ),
                None,
            )
            if generated_uid_index is None:
                fail("service_user_generated_uid_failure_write_missing")
            if any(
                "|PrimaryGroupID|" in command
                or "|NFSHomeDirectory|" in command
                or "|UserShell|" in command
                or "|IsHidden|" in command
                or "|Password|" in command
                for command in commands[generated_uid_index + 1 :]
            ):
                fail("service_user_generated_uid_failure_continued_after_write")
            return commands

        if result.returncode != 0:
            fail(
                "service_user_generated_uid_create_rejected:"
                f"{result.returncode}:{result.stderr.strip()}"
            )
        if result.stderr:
            fail("service_user_generated_uid_create_diagnostic")
        if result.stdout != f"generated_uid={generated_uid}\n":
            fail("service_user_generated_uid_output_invalid")
        generated_uid_commands = [
            command
            for command in commands
            if command.startswith(
                ".|-create|/Users/scratchbird|GeneratedUID|"
            )
        ]
        if generated_uid_commands != [
            f".|-create|/Users/scratchbird|GeneratedUID|{generated_uid}|"
        ]:
            fail("service_user_generated_uid_write_mismatch")
        if ".|-delete|/Users/scratchbird|" in commands:
            fail("service_user_generated_uid_unexpected_cleanup")
        unique_id_command = ".|-create|/Users/scratchbird|UniqueID|501|"
        generated_uid_command = (
            f".|-create|/Users/scratchbird|GeneratedUID|{generated_uid}|"
        )
        primary_group_command = ".|-create|/Users/scratchbird|PrimaryGroupID|700|"
        try:
            unique_id_index = commands.index(unique_id_command)
            generated_uid_index = commands.index(generated_uid_command)
            primary_group_index = commands.index(primary_group_command)
        except ValueError:
            fail("service_user_generated_uid_attribute_order_missing")
        if not unique_id_index < generated_uid_index < primary_group_index:
            fail("service_user_generated_uid_attribute_order_invalid")
        return commands

    successful_commands = run_case("success", False)
    if successful_commands[-1] != ".|-create|/Users/scratchbird|Password|*|":
        fail("service_user_creation_attribute_sequence_incomplete")
    run_case("generated-uid-create-failure", True)
    return {
        "generated_uid_creation": "passed",
        "generated_uid_readback_value": generated_uid,
        "generated_uid_failure_cleanup": "passed",
    }


def run_system_installer_diagnostic_smoke(
    helper: Path,
    work_root: Path,
) -> dict[str, str]:
    """Prove the packaged helper logs only fixed system-install diagnostics."""

    helper_text = helper.read_text(encoding="utf-8")
    function_start = helper_text.find("diagnostic() {")
    next_block = helper_text.find("\nwhile [ \"$#\" -gt 0 ]; do", function_start)
    if function_start < 0 or next_block < 0:
        fail("system_diagnostic_helper_functions_missing")
    function_text = helper_text[function_start:next_block].rstrip()
    if function_text.count("/usr/bin/logger") != 2:
        fail("system_diagnostic_logger_contract_missing")

    fixture_root = work_root / "system-installer-diagnostic"
    fixture_bin = fixture_root / "bin"
    fixture_bin.mkdir(parents=True)
    logger = fixture_bin / "logger"
    logger.write_text(
        "#!/bin/sh\n"
        "set -eu\n"
        "printf '%s|' \"$@\" >> \"${SB_LOGGER_LOG:?}\"\n"
        "printf '\\n' >> \"${SB_LOGGER_LOG:?}\"\n"
        "exit \"${SB_LOGGER_STATUS:-0}\"\n",
        encoding="utf-8",
    )
    logger.chmod(0o755)
    function_text = function_text.replace("/usr/bin/logger", str(logger))

    harness = fixture_root / "system-installer-diagnostic-harness.sh"
    harness.write_text(
        "#!/bin/sh\n"
        "set -eu\n"
        "ACTION=${SB_ACTION:-post-install}\n"
        "install_root=${SB_INSTALL_ROOT:-/}\n"
        "identity_mode=${SB_IDENTITY_MODE:-system}\n"
        "identity_validation_stage=${SB_IDENTITY_VALIDATION_STAGE:-}\n"
        "install_validation_stage=${SB_INSTALL_VALIDATION_STAGE:-launchd_sbmgr_definition_validation}\n"
        "installer_failure_reported=false\n\n"
        f"{function_text}\n"
        "case ${SB_DIAGNOSTIC_CASE:?} in\n"
        "    fail)\n"
        "        trap 'report_unclassified_install_failure \"$?\"' 0\n"
        "        fail \"${SB_DIAGNOSTIC_CODE:-BOOTSTRAP.DIRECTORY_PERMISSION_INVALID}\"\n"
        "        ;;\n"
        "    raw-log)\n"
        "        log_system_installer_diagnostic \"${SB_DIAGNOSTIC_CODE:?}\"\n"
        "        ;;\n"
        "    bare-failure)\n"
        "        trap 'report_unclassified_install_failure \"$?\"' 0\n"
        "        false\n"
        "        ;;\n"
        "    *)\n"
        "        exit 64\n"
        "        ;;\n"
        "esac\n",
        encoding="utf-8",
    )
    harness.chmod(0o755)

    def run_case(
        name: str,
        diagnostic_case: str,
        *,
        code: str = "BOOTSTRAP.DIRECTORY_PERMISSION_INVALID",
        stage: str = "launchd_sbmgr_definition_validation",
        identity_mode: str = "system",
        logger_status: int = 0,
    ) -> tuple[subprocess.CompletedProcess[str], list[str]]:
        log_path = fixture_root / f"{name}.logger.log"
        environment = os.environ.copy()
        environment.update(
            {
                "SB_LOGGER_LOG": str(log_path),
                "SB_LOGGER_STATUS": str(logger_status),
                "SB_DIAGNOSTIC_CASE": diagnostic_case,
                "SB_DIAGNOSTIC_CODE": code,
                "SB_INSTALL_VALIDATION_STAGE": stage,
                "SB_IDENTITY_MODE": identity_mode,
            }
        )
        result = subprocess.run(
            ["/bin/sh", str(harness)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            env=environment,
        )
        logs = (
            log_path.read_text(encoding="utf-8").splitlines()
            if log_path.exists()
            else []
        )
        return result, logs

    stage = "launchd_sbmgr_definition_validation"
    code = "BOOTSTRAP.DIRECTORY_PERMISSION_INVALID"
    expected_log = (
        "-t|scratchbird-installer|"
        f"scratchbird-installer stage={stage} code={code}|"
    )
    allowed, allowed_logs = run_case("allowed", "fail")
    if allowed.returncode != 1:
        fail("system_diagnostic_allowed_failure_status")
    if allowed.stdout:
        fail("system_diagnostic_allowed_failure_stdout")
    if allowed.stderr != (
        f"{code}\nBOOTSTRAP.MACOS_INSTALL_VALIDATION_STAGE={stage}\n"
    ):
        fail("system_diagnostic_allowed_failure_stderr")
    if allowed_logs != [expected_log]:
        fail("system_diagnostic_allowed_failure_log")

    fixture, fixture_logs = run_case(
        "fixture",
        "fail",
        identity_mode="fixture",
    )
    if fixture.returncode != 1 or fixture.stdout:
        fail("system_diagnostic_fixture_failure_status")
    if fixture.stderr != f"{code}\n" or fixture_logs:
        fail("system_diagnostic_fixture_failure_logged")

    untrusted_code, untrusted_code_logs = run_case(
        "untrusted-code",
        "raw-log",
        code="BOOTSTRAP.UNTRUSTED_/private/input",
    )
    if (
        untrusted_code.returncode != 0
        or untrusted_code.stdout
        or untrusted_code.stderr
        or untrusted_code_logs
    ):
        fail("system_diagnostic_untrusted_code_logged")

    untrusted_stage, untrusted_stage_logs = run_case(
        "untrusted-stage",
        "raw-log",
        stage="untrusted_/private/input",
    )
    if (
        untrusted_stage.returncode != 0
        or untrusted_stage.stdout
        or untrusted_stage.stderr
        or untrusted_stage_logs
    ):
        fail("system_diagnostic_untrusted_stage_logged")

    logger_failure, logger_failure_logs = run_case(
        "logger-failure",
        "fail",
        logger_status=1,
    )
    if logger_failure.returncode != 1 or logger_failure.stdout:
        fail("system_diagnostic_logger_failure_status")
    if logger_failure.stderr != allowed.stderr:
        fail("system_diagnostic_logger_failure_stderr")
    if logger_failure_logs != [expected_log]:
        fail("system_diagnostic_logger_failure_log")

    fallback, fallback_logs = run_case("fallback", "bare-failure")
    fallback_code = "BOOTSTRAP.MACOS_INSTALL_STAGE_UNCLASSIFIED_FAILURE"
    expected_fallback_log = (
        "-t|scratchbird-installer|"
        f"scratchbird-installer stage={stage} code={fallback_code}|"
    )
    if fallback.returncode != 1 or fallback.stdout or fallback.stderr:
        fail("system_diagnostic_unclassified_failure_status")
    if fallback_logs != [expected_fallback_log]:
        fail("system_diagnostic_unclassified_failure_log")

    for line in [*allowed_logs, *logger_failure_logs, *fallback_logs]:
        for forbidden in ("/private/", "untrusted", "60000", "0.0.0"):
            if forbidden in line:
                fail("system_diagnostic_sensitive_value_logged")
    return {
        "allowed_fixed_code_and_stage": "passed",
        "fixture_mode_silent": "passed",
        "untrusted_values_suppressed": "passed",
        "logger_failure_non_interference": "passed",
        "unclassified_set_e_failure_fallback": "passed",
        "fail_trap_no_duplicate": "passed",
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
        "require_existing_directory_if_present /opt",
        "require_existing_file_if_present /opt/ScratchBird/bin/SBlaunch 755",
        "/Library/LaunchDaemons/com.scratchbird.sbsrv.plist 644",
        "/bin/launchctl print system",
        '/bin/launchctl print "system/$label"',
        "com.scratchbird.sbsrv com.scratchbird.sbmgr",
    ):
        if fragment not in preinstall_text:
            fail(f"pkg_preinstall_contract_missing:{fragment}")
    text = postinstall.read_text(encoding="utf-8")
    required = (
        "/opt/ScratchBird/libexec/scratchbird-macos-system-install",
        "PATH=/usr/bin:/bin:/usr/sbin:/sbin",
        "postinstall_stage=postinstall_wrapper_validation",
        "log_postinstall_diagnostic()",
        "/usr/bin/logger -t scratchbird-installer",
        '"scratchbird-installer stage=$postinstall_stage code=$1"',
        "fail() {",
        "fail BOOTSTRAP.OS_AUTHORITY_DENIED",
        "require_no_extended_acl",
        "require_root_executable",
        "root:wheel:755",
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
        (server, "default_path = /var/lib/scratchbird/data/default.sbdb"),
        (server, "log_file = /var/log/scratchbird/runtime/SBsrv.log"),
        (server, "executable_path = /opt/ScratchBird/bin/SBgate"),
        (
            server,
            "sbps_endpoint = /var/run/scratchbird/sb_server/control/"
            "sb_server.sbps.sock",
        ),
        (listener, "port = 3092"),
        (listener, "parser_executable = /opt/ScratchBird/bin/SBParser"),
        (manager, "manager.proxy.port = 3092"),
        (manager, "manager.log.path = /var/log/scratchbird/runtime/SBmgr.log"),
        (manager, "manager.backend.native_port = 0"),
        (parser, "parser.worker_binary = /opt/ScratchBird/bin/SBParser"),
    )
    for content, fragment in required_fragments:
        if fragment not in content:
            fail(f"installed_config_contract_missing:{fragment}")
    if "server.listener.native" in server:
        fail("installed_config_legacy_listener_profile_present")
    if has_listener_profile_section(server):
        fail("installed_config_unconfigured_listener_profile_present")
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
    launcher = portable_bin / "SBlaunch"
    launcher.write_bytes(b"fixture launcher\n")
    launcher.chmod(0o755)
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
    service_user_creation = run_service_user_creation_smoke(
        staged_helper,
        work_root,
    )
    diagnostic_logging = run_system_installer_diagnostic_smoke(
        staged_helper,
        work_root,
    )
    staged_launcher = system_root / "opt" / "ScratchBird" / "bin" / "SBlaunch"
    if not staged_launcher.is_file() or staged_launcher.stat().st_size <= 0:
        fail("builder_service_launcher_not_staged")
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
        if payload.get("UserName") != "root":
            fail(f"builder_launchd_user_mismatch:{path.name}")
        if payload.get("GroupName") != "wheel":
            fail(f"builder_launchd_group_mismatch:{path.name}")
        if payload.get("InitGroups") is not False:
            fail(f"builder_launchd_init_groups_mismatch:{path.name}")
        selector = {
            "com.scratchbird.sbsrv.plist": "sbsrv",
            "com.scratchbird.sbmgr.plist": "sbmgr",
        }[path.name]
        if payload.get("ProgramArguments") != [MACOS_SERVICE_LAUNCHER, selector]:
            fail(f"builder_launchd_selector_mismatch:{path.name}")
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
        "service_user_creation": service_user_creation,
        "diagnostic_logging": diagnostic_logging,
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
        "resolved_effective_group_policy": MACOS_SERVICE_PROCESS_GROUP_POLICY,
        "service_launcher": MACOS_SERVICE_LAUNCHER,
        "launchd_bootstrap_identity": "root:wheel",
        "launchd_init_groups": False,
        "final_product_identity": "scratchbird:scratchbird",
        "final_supplementary_groups": [],
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
    launcher = asset_root / "scratchbird-macos-service-launcher.c"
    proof: dict[str, Any] = {
        "schema_id": "scratchbird.macos_system_install_smoke.v1",
        "profile": validate_profile(asset_root),
        "launchd": validate_launchd(asset_root),
        "service_launcher": validate_launcher_static(launcher),
        "pkg_scripts": validate_pkg_scripts(asset_root),
    }
    validate_helper_static(helper)
    proof["checkmember_negative_status"] = run_checkmember_negative_status_smoke(
        helper,
        work_root,
    )
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
