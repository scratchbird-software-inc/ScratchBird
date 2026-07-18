#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Verify Linux system-package layout and lifecycle without requiring root."""

from __future__ import annotations

import argparse
from io import BytesIO
import json
import os
from pathlib import Path
import secrets
import shutil
import stat
import subprocess
import sys
import tarfile
from typing import Any


SYSTEM_FILES = {
    "usr/lib/systemd/system/scratchbird-sbsrv.service": 0o644,
    "usr/lib/sysusers.d/scratchbird.conf": 0o644,
    "usr/lib/tmpfiles.d/scratchbird.conf": 0o644,
    "usr/lib/scratchbird/scratchbird-system-install": 0o755,
    "opt/ScratchBird/share/scratchbird/release/LINUX_SYSTEM_INSTALL_PROFILE.json": 0o644,
}
CONFIG_FILES = (
    "etc/scratchbird/SBsrv.conf",
    "etc/scratchbird/SBgate.conf",
    "etc/scratchbird/SBmgr.conf",
    "etc/scratchbird/SBParser.conf",
    "etc/scratchbird/SBbootstrap.profile",
)
SYSTEM_CONFIG_TOKENS = {
    "etc/scratchbird/SBsrv.conf": (
        "data_dir = /run/scratchbird/runtime",
        "control_dir = /run/scratchbird/control",
        "log_file = /var/log/scratchbird/SBsrv.log",
        "default_path = /var/lib/scratchbird/data/default.sbdb",
        "executable_path = /opt/ScratchBird/bin/SBgate",
        "control_dir = /run/scratchbird/listener/control",
        "runtime_dir = /run/scratchbird/listener/runtime",
        "sbps_endpoint = /run/scratchbird/control/sb_server.sbps.sock",
    ),
    "etc/scratchbird/SBgate.conf": (
        "parser_executable = /opt/ScratchBird/bin/SBParser",
        "server_endpoint = /run/scratchbird/control/sb_server.sbps.sock",
        "port = 3092",
        "control_dir = /run/scratchbird/listener/control",
        "runtime_dir = /run/scratchbird/listener/runtime",
    ),
    "etc/scratchbird/SBmgr.conf": (
        "manager.runtime_dir = /run/scratchbird/manager/runtime",
        "manager.control_dir = /run/scratchbird/manager/control",
        "manager.log.path = /var/log/scratchbird/SBmgr.log",
        "manager.proxy.port = 3092",
        "manager.owner.database_path = /var/lib/scratchbird/data/default.sbdb",
    ),
    "etc/scratchbird/SBParser.conf": (
        "parser.worker_binary = /opt/ScratchBird/bin/SBParser",
        "parser.execution.engine_transport = sbps_ipc_only",
        "parser.execution.direct_engine_link = forbidden",
        "parser.execution.cross_parser_dependency = forbidden",
    ),
    "etc/scratchbird/SBbootstrap.profile": (
        "platform = linux",
        "service_identity = scratchbird",
        "service_group = scratchbird",
    ),
}
FORBIDDEN_SYSTEM_CONFIG_TOKENS = (
    "operator_required",
    "parser.worker_binary = bin/SBParser",
    "parser_executable = bin/SBParser",
    "executable_path = bin/SBgate",
    "parser_executable_path = bin/SBParser",
    "server.listener.native",
    "server_endpoint = runtime/",
    "control_dir = runtime/",
    "runtime_dir = runtime/",
    "manager.runtime_dir = runtime/",
    "manager.control_dir = runtime/",
    "manager.owner.database_path = data/",
    "default_path = data/",
)
DATABASE_SUFFIXES = (".sbdb", ".sbrd")
SECURITY_SIDECAR_MARKERS = (
    ".security_principal_events",
    ".local_password_auth",
)
BOOTSTRAP_TOOL_PATHS = {
    "sbsql": Path("/opt/ScratchBird/bin/SBsql"),
    "sbsec": Path("/opt/ScratchBird/bin/SBsec"),
}
BOOTSTRAP_PROFILE_PATH = Path("/etc/scratchbird/SBbootstrap.profile")
BOOTSTRAP_RESOURCE_PACK_ROOT = Path(
    "/opt/ScratchBird/share/scratchbird/resources/seed-packs/initial-resource-pack"
)
BOOTSTRAP_POLICY_PACK_ROOT = Path(
    "/opt/ScratchBird/share/scratchbird/resources/policy-packs/default-local-password"
)
BOOTSTRAP_DATA_ROOT = Path("/var/lib/scratchbird/data")
BOOTSTRAP_DATABASE_NAME = "installer-first-principal.smoke.sbdb"
BOOTSTRAP_PRINCIPAL = "installer_qa_admin"
BOOTSTRAP_PACKAGE_FILES = (
    "opt/ScratchBird/bin/SBsql",
    "opt/ScratchBird/bin/SBsec",
    "etc/scratchbird/SBbootstrap.profile",
    "opt/ScratchBird/share/scratchbird/resources/seed-packs/"
    "initial-resource-pack/RESOURCE_SEED_MANIFEST.csv",
    "opt/ScratchBird/share/scratchbird/resources/policy-packs/"
    "default-local-password/POLICY_PACK_MANIFEST.json",
)


def fail(message: str) -> None:
    print(f"smoke_install_linux_system=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def has_listener_profile_section(text: str) -> bool:
    return any(
        line.strip().startswith("[server.listener.profile.")
        and line.strip().endswith("]")
        for line in text.splitlines()
    )


def ar_member(path: Path, requested_name: str) -> bytes:
    data = path.read_bytes()
    if not data.startswith(b"!<arch>\n"):
        fail(f"not_ar_archive:{path.name}")
    offset = 8
    while offset < len(data):
        header = data[offset : offset + 60]
        if len(header) != 60 or header[58:60] != b"`\n":
            fail(f"invalid_ar_header:{path.name}:offset={offset}")
        name = header[:16].decode("ascii").strip().rstrip("/")
        try:
            size = int(header[48:58].decode("ascii").strip())
        except ValueError:
            fail(f"invalid_ar_size:{path.name}:offset={offset}")
        start = offset + 60
        payload = data[start : start + size]
        if len(payload) != size:
            fail(f"truncated_ar_member:{path.name}:{name}")
        if name == requested_name:
            return payload
        offset = start + size + (size % 2)
    fail(f"ar_member_missing:{path.name}:{requested_name}")


def tar_members(payload: bytes) -> tuple[tarfile.TarFile, dict[str, tarfile.TarInfo]]:
    archive = tarfile.open(fileobj=BytesIO(payload), mode="r:gz")
    members = {member.name.removeprefix("./"): member for member in archive.getmembers()}
    return archive, members


def member_bytes(
    archive: tarfile.TarFile,
    members: dict[str, tarfile.TarInfo],
    name: str,
) -> bytes:
    member = members.get(name)
    if member is None or not member.isfile():
        fail(f"package_file_missing:{name}")
    handle = archive.extractfile(member)
    if handle is None:
        fail(f"package_file_unreadable:{name}")
    return handle.read()


def member_text(
    archive: tarfile.TarFile,
    members: dict[str, tarfile.TarInfo],
    name: str,
) -> str:
    try:
        return member_bytes(archive, members, name).decode("utf-8")
    except UnicodeDecodeError:
        fail(f"package_text_not_utf8:{name}")


def exactly_one(root: Path, pattern: str) -> Path:
    matches = sorted(root.glob(pattern))
    if len(matches) != 1:
        fail(f"expected_one:{pattern}:count={len(matches)}")
    return matches[0]


def verify_package_evidence(root: Path) -> dict[str, Any]:
    path = root / "LINUX_SYSTEM_PACKAGE_EVIDENCE.json"
    if not path.is_file():
        fail("linux_system_package_evidence_missing")
    evidence = json.loads(path.read_text(encoding="utf-8"))
    identity = evidence.get("os_identity")
    if not isinstance(identity, dict):
        fail("linux_system_package_evidence_identity_missing")
    if identity.get("service_effective_group_policy") != (
        "only_scratchbird_effective_group"
    ):
        fail("linux_system_package_evidence_exact_group_policy_missing")
    if identity.get("service_authority_scope") != (
        "filesystem_directory_and_process_execution_only_"
        "no_database_or_security_authority"
    ):
        fail("linux_system_package_evidence_authority_scope_mismatch")
    return {
        "path": path.name,
        "service_effective_group_policy": (
            "only_scratchbird_effective_group"
        ),
        "result": "passed",
    }


def check_no_database_or_security_sidecars(names: set[str], source: str) -> None:
    for name in sorted(names):
        lower = name.lower()
        if lower.endswith(DATABASE_SUFFIXES):
            fail(f"database_file_created_or_packaged:{source}:{name}")
        if any(marker in lower for marker in SECURITY_SIDECAR_MARKERS):
            fail(f"security_sidecar_created_or_packaged:{source}:{name}")


def verify_bootstrap_package_payload(
    members: dict[str, tarfile.TarInfo],
) -> dict[str, Any]:
    """Require every installed input used by explicit first-principal bootstrap.

    The privileged test below proves executable behavior.  This archive check
    keeps the package test fail-closed before a host mutation is requested and
    makes the bootstrap inputs part of the portable artifact contract as well.
    """

    for path in BOOTSTRAP_PACKAGE_FILES:
        member = members.get(path)
        if member is None or not member.isfile():
            fail(f"bootstrap_package_input_missing:{path}")
    return {
        "tools": ["SBsql", "SBsec"],
        "profile": "/etc/scratchbird/SBbootstrap.profile",
        "resource_seed_pack": str(BOOTSTRAP_RESOURCE_PACK_ROOT),
        "policy_seed_pack": str(BOOTSTRAP_POLICY_PACK_ROOT),
        "result": "present",
    }


def verify_portable_tar(portable_tar: Path) -> dict[str, Any]:
    with tarfile.open(portable_tar, mode="r:gz") as archive:
        names = {member.name.removeprefix("./") for member in archive.getmembers()}
    forbidden_prefixes = (
        "usr/lib/systemd/",
        "usr/lib/sysusers.d/",
        "usr/lib/tmpfiles.d/",
        "usr/lib/scratchbird/scratchbird-system-install",
        "var/lib/scratchbird/",
        "var/log/scratchbird/",
        "run/scratchbird/",
    )
    for name in names:
        if name.startswith(forbidden_prefixes):
            fail(f"portable_tar_has_system_lifecycle:{name}")
    check_no_database_or_security_sidecars(names, "portable_tar")
    return {
        "archive": portable_tar.name,
        "members": len(names),
        "system_lifecycle": "absent",
    }


def verify_deb(deb: Path, work_root: Path) -> tuple[dict[str, Any], bytes]:
    data_payload = ar_member(deb, "data.tar.gz")
    control_payload = ar_member(deb, "control.tar.gz")
    data_archive, data_members = tar_members(data_payload)
    control_archive, control_members = tar_members(control_payload)
    try:
        names = set(data_members)
        check_no_database_or_security_sidecars(names, "deb")
        bootstrap_payload = verify_bootstrap_package_payload(data_members)
        if any(name.startswith("etc/systemd/system/") and ".wants/" in name for name in names):
            fail("deb_service_enabled_by_payload_symlink")
        packaged_service_units = {
            name
            for name in names
            if name.startswith("usr/lib/systemd/system/")
            and name.endswith(".service")
        }
        if packaged_service_units != {
            "usr/lib/systemd/system/scratchbird-sbsrv.service"
        }:
            fail(
                "deb_top_level_service_set_mismatch:"
                + ",".join(sorted(packaged_service_units))
            )

        for name, expected_mode in SYSTEM_FILES.items():
            member = data_members.get(name)
            if member is None or not member.isfile():
                fail(f"deb_system_file_missing:{name}")
            if member.uid != 0 or member.gid != 0:
                fail(f"deb_system_file_owner_not_root:{name}:{member.uid}:{member.gid}")
            if stat.S_IMODE(member.mode) != expected_mode:
                fail(
                    f"deb_system_file_mode:{name}:"
                    f"{stat.S_IMODE(member.mode):04o}!={expected_mode:04o}"
                )
        for name in CONFIG_FILES:
            member = data_members.get(name)
            if member is None or not member.isfile():
                fail(f"deb_config_missing:{name}")
            if member.uid != 0 or member.gid != 0:
                fail(f"deb_config_archive_owner_not_root:{name}:{member.uid}:{member.gid}")
            if stat.S_IMODE(member.mode) != 0o640:
                fail(f"deb_config_mode:{name}:{stat.S_IMODE(member.mode):04o}")

        unit = member_text(data_archive, data_members, "usr/lib/systemd/system/scratchbird-sbsrv.service")
        required_unit_lines = (
            "User=scratchbird",
            "Group=scratchbird",
            "ExecStart=/opt/ScratchBird/bin/SBsrv --config /etc/scratchbird/SBsrv.conf --foreground",
            "NoNewPrivileges=true",
            "ProtectSystem=strict",
            "ReadWritePaths=/var/lib/scratchbird /var/log/scratchbird /run/scratchbird",
        )
        for line in required_unit_lines:
            if line not in unit:
                fail(f"systemd_unit_contract_missing:{line}")
        if "--create-if-missing" in unit:
            fail("systemd_unit_create_if_missing_forbidden")

        sysusers = member_text(data_archive, data_members, "usr/lib/sysusers.d/scratchbird.conf")
        if "g      scratchbird" not in sysusers or "u      scratchbird" not in sysusers:
            fail("sysusers_identity_missing")
        if "nologin" not in sysusers:
            fail("sysusers_non_login_shell_missing")

        tmpfiles = member_text(data_archive, data_members, "usr/lib/tmpfiles.d/scratchbird.conf")
        for directory in (
            "/var/lib/scratchbird/data",
            "/var/log/scratchbird",
            "/run/scratchbird/control",
            "/run/scratchbird/runtime",
            "/run/scratchbird/listener/control",
            "/run/scratchbird/listener/runtime",
            "/run/scratchbird/manager/control",
            "/run/scratchbird/manager/runtime",
        ):
            if f"d {directory}" not in tmpfiles or "0750" not in next(
                line for line in tmpfiles.splitlines() if line.startswith(f"d {directory} ")
            ):
                fail(f"tmpfiles_directory_contract_missing:{directory}")

        compatibility_port = str(3000 + 50)
        for name in CONFIG_FILES:
            text = member_text(data_archive, data_members, name)
            if compatibility_port in text:
                fail(f"compatibility_port_in_native_system_config:{name}")
            for token in SYSTEM_CONFIG_TOKENS[name]:
                if token not in text:
                    fail(f"system_config_token_missing:{name}:{token}")
            for token in FORBIDDEN_SYSTEM_CONFIG_TOKENS:
                if token in text:
                    fail(f"relative_or_placeholder_system_config:{name}:{token}")
            if name == "etc/scratchbird/SBsrv.conf" and has_listener_profile_section(text):
                fail("unconfigured_listener_profile_in_system_defaults")

        profile = json.loads(
            member_text(
                data_archive,
                data_members,
                "opt/ScratchBird/share/scratchbird/release/LINUX_SYSTEM_INSTALL_PROFILE.json",
            )
        )
        if profile.get("schema_id") != "scratchbird.linux_system_install_profile.v1":
            fail("linux_system_profile_schema_mismatch")
        if profile.get("native_default_port") != 3092:
            fail("linux_system_profile_native_port_mismatch")
        profile_paths = {
            row.get("path")
            for row in profile.get("directories", [])
            if isinstance(row, dict)
        }
        for required_path in (
            "/var/lib/scratchbird/data",
            "/var/log/scratchbird",
            "/run/scratchbird/control",
            "/run/scratchbird/runtime",
            "/run/scratchbird/listener/control",
            "/run/scratchbird/listener/runtime",
            "/run/scratchbird/manager/control",
            "/run/scratchbird/manager/runtime",
        ):
            if required_path not in profile_paths:
                fail(f"linux_system_profile_directory_missing:{required_path}")
        identity = profile.get("os_identity")
        if not isinstance(identity, dict):
            fail("linux_system_profile_identity_missing")
        if identity.get("human_service_group_membership_mutation") != "forbidden":
            fail("linux_system_profile_human_membership_mutation_not_forbidden")
        if identity.get("create_time_os_authorization") != "root_only":
            fail("linux_system_profile_create_authority_not_root_only")
        if identity.get("service_authority_scope") != (
            "filesystem_directory_and_process_execution_only_"
            "no_database_or_security_authority"
        ):
            fail("linux_system_profile_service_authority_scope_mismatch")
        if "only_scratchbird_effective_group" not in identity.get(
            "existing_identity_validation", []
        ):
            fail("linux_system_profile_exact_service_group_policy_missing")
        if identity.get("package_manager_never_adds_human_group_members") is not True:
            fail("linux_system_profile_human_membership_guard_missing")
        service = profile.get("service")
        if not isinstance(service, dict):
            fail("linux_system_profile_service_missing")
        if service.get("default_enablement") != "disabled" or service.get("default_activity") != "not_started":
            fail("linux_system_profile_default_service_state_mismatch")
        if service.get("binary") != "/opt/ScratchBird/bin/SBsrv":
            fail("linux_system_profile_top_level_binary_mismatch")
        if service.get("owns_listener_and_parser_children") is not True:
            fail("linux_system_profile_child_ownership_missing")
        if service.get("create_if_missing") is not False:
            fail("linux_system_profile_database_auto_create_not_false")
        lifecycle = profile.get("lifecycle")
        if not isinstance(lifecycle, dict):
            fail("linux_system_profile_lifecycle_missing")
        if lifecycle.get("database_files_created") is not False:
            fail("linux_system_profile_database_creation_not_false")
        if lifecycle.get("security_sidecars_created") is not False:
            fail("linux_system_profile_sidecar_creation_not_false")
        if lifecycle.get("human_service_group_membership_mutation") is not False:
            fail("linux_system_profile_lifecycle_human_membership_mutation")

        postinst = member_text(control_archive, control_members, "postinst")
        prerm = member_text(control_archive, control_members, "prerm")
        postrm = member_text(control_archive, control_members, "postrm")
        conffiles = member_text(control_archive, control_members, "conffiles")
        if "scratchbird-system-install post-install" not in postinst:
            fail("deb_postinst_lifecycle_missing")
        if "scratchbird-system-install pre-remove" not in prerm:
            fail("deb_prerm_lifecycle_missing")
        for forbidden in ("systemctl start", "systemctl enable", "systemctl preset", "--now"):
            if forbidden in postinst:
                fail(f"deb_postinst_service_activation_forbidden:{forbidden}")
        if "daemon-reload" not in postrm:
            fail("deb_postrm_daemon_reload_missing")
        for required_restore in (
            "config-preserve",
            "cp -an",
            "LINUX_SYSTEM_UNINSTALL_STATE.json",
            '"${1:-}" = remove',
        ):
            if required_restore not in postrm:
                fail(f"deb_postrm_config_preservation_missing:{required_restore}")
        for config in CONFIG_FILES:
            if f"/{config}" not in conffiles:
                fail(f"deb_conffile_missing:{config}")

        helper = member_bytes(
            data_archive,
            data_members,
            "usr/lib/scratchbird/scratchbird-system-install",
        )
        helper_text = helper.decode("utf-8")
        for identity_guard in (
            "/etc/passwd",
            "/etc/group",
            "/etc/shadow",
            "must be uniquely local",
            "must not have root authority",
            "service home mismatch",
            "service account is not locked",
            'id -G "$SERVICE_USER"',
            "supplementary group authority is forbidden",
            "SYS_UID_MAX",
            "numeric identity is shared",
        ):
            if identity_guard not in helper_text:
                fail(f"service_identity_guard_missing:{identity_guard}")
        for forbidden_database_action in (
            "SBsec",
            "SBsrv",
            "--create-if-missing",
            ".sbdb",
            ".sbrd",
            "security_principal_events",
            "local_password_auth",
        ):
            if forbidden_database_action in helper_text:
                fail(
                    "linux_lifecycle_database_action_forbidden:"
                    f"{forbidden_database_action}"
                )
        for forbidden_membership_action in (
            "--installer-user",
            "usermod",
            "gpasswd",
            "GROUP_MEMBERSHIP_REQUIRED",
        ):
            if forbidden_membership_action in helper_text:
                fail(
                    "linux_lifecycle_human_membership_action_forbidden:"
                    f"{forbidden_membership_action}"
                )
    finally:
        data_archive.close()
        control_archive.close()

    fixture = work_root / "lifecycle-fixture"
    if fixture.exists():
        shutil.rmtree(fixture)
    (fixture / "etc" / "scratchbird").mkdir(parents=True)
    (fixture / "etc" / "scratchbird" / "fixture.conf").write_text(
        "port = 3092\n", encoding="utf-8"
    )
    helper_path = work_root / "scratchbird-system-install"
    helper_path.write_bytes(helper)
    helper_path.chmod(0o755)
    helper_command = [
        str(helper_path),
        "post-install",
        "--root",
        str(fixture.resolve()),
        "--identity-mode",
        "fixture",
        "--package-format",
        "fixture",
        "--package-version",
        "0.0.0-smoke",
    ]
    result = subprocess.run(
        helper_command,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode != 0:
        fail(f"lifecycle_fixture_failed:{result.returncode}:{result.stdout.strip()}")
    repeat = subprocess.run(
        helper_command,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if repeat.returncode != 0:
        fail(f"lifecycle_fixture_not_idempotent:{repeat.returncode}:{repeat.stdout.strip()}")
    invalid = subprocess.run(
        [*helper_command, "--installer-user", "fixtureoperator"],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if invalid.returncode == 0 or "BOOTSTRAP.INSTALL_DEFAULTS_INVALID" not in invalid.stdout:
        fail("lifecycle_fixture_installer_user_surface_not_refused")
    required_dirs = (
        "var/lib/scratchbird/data",
        "var/lib/scratchbird/install",
        "var/log/scratchbird",
        "run/scratchbird/control",
        "run/scratchbird/runtime",
        "run/scratchbird/listener/control",
        "run/scratchbird/listener/runtime",
        "run/scratchbird/manager/control",
        "run/scratchbird/manager/runtime",
    )
    for rel in required_dirs:
        directory = fixture / rel
        if not directory.is_dir():
            fail(f"lifecycle_fixture_directory_missing:{rel}")
        if stat.S_IMODE(directory.stat().st_mode) != 0o750:
            fail(f"lifecycle_fixture_directory_mode:{rel}")
    state_path = fixture / "var/lib/scratchbird/install/LINUX_SYSTEM_INSTALL_STATE.json"
    state = json.loads(state_path.read_text(encoding="utf-8"))
    if state.get("native_default_port") != 3092:
        fail("lifecycle_fixture_native_port_mismatch")
    if state.get("database_files_created") is not False:
        fail("lifecycle_fixture_created_database")
    if state.get("security_sidecars_created") is not False:
        fail("lifecycle_fixture_created_security_sidecar")
    if state.get("human_service_group_membership_mutated") is not False:
        fail("lifecycle_fixture_human_membership_mutated")
    if state.get("create_time_os_authorization") != "root_only":
        fail("lifecycle_fixture_create_authority_not_root_only")
    if state.get("service_authority_scope") != (
        "filesystem_directory_and_process_execution_only_"
        "no_database_or_security_authority"
    ):
        fail("lifecycle_fixture_service_authority_scope_mismatch")
    pre_remove = subprocess.run(
        [
            str(helper_path),
            "pre-remove",
            "--root",
            str(fixture.resolve()),
            "--identity-mode",
            "fixture",
            "--package-format",
            "fixture",
            "--package-version",
            "0.0.0-smoke",
        ],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if pre_remove.returncode != 0:
        fail(f"lifecycle_fixture_pre_remove_failed:{pre_remove.stdout.strip()}")
    preserved_config = (
        fixture / "var/lib/scratchbird/install/config-preserve/fixture.conf"
    )
    if not preserved_config.is_file():
        fail("lifecycle_fixture_config_preservation_missing")
    fixture_names = {
        path.relative_to(fixture).as_posix()
        for path in fixture.rglob("*")
        if path.is_file()
    }
    check_no_database_or_security_sidecars(fixture_names, "lifecycle_fixture")

    return (
        {
            "archive": deb.name,
            "payload_members": len(data_members),
            "control_members": sorted(control_members),
            "lifecycle_fixture": "passed",
            "lifecycle_fixture_idempotence": "passed",
            "installer_user_surface_refusal": "passed",
            "human_service_group_membership_mutation": "forbidden",
            "pre_remove_config_preservation": "passed",
            "service_default": "disabled_not_started",
            "native_default_port": 3092,
            "first_principal_bootstrap_payload": bootstrap_payload,
        },
        data_payload,
    )


def verify_rpm_recipe(root: Path) -> dict[str, Any]:
    recipe = root / "scratchbird-linux-system.spec"
    if not recipe.is_file():
        fail("rpm_system_recipe_missing")
    text = recipe.read_text(encoding="utf-8")
    required = (
        "scratchbird-system-install post-install",
        "scratchbird-system-install pre-remove",
        "/usr/lib/systemd/system/scratchbird-sbsrv.service",
        "%config(noreplace) /etc/scratchbird/SBsrv.conf",
        "Requires: systemd",
        "Requires: shadow-utils",
    )
    for fragment in required:
        if fragment not in text:
            fail(f"rpm_recipe_contract_missing:{fragment}")
    for forbidden in ("%systemd_post", "systemctl start", "systemctl enable", "systemctl preset", "--now"):
        if forbidden in text:
            fail(f"rpm_recipe_service_activation_forbidden:{forbidden}")
    for required_restore in ("config-preserve", "cp -an", "LINUX_SYSTEM_UNINSTALL_STATE.json"):
        if required_restore not in text:
            fail(f"rpm_recipe_config_preservation_missing:{required_restore}")
    return {"recipe": recipe.name, "lifecycle": "present"}


def verify_aur(root: Path) -> dict[str, Any]:
    bundle = exactly_one(root, "scratchbird-aur-*.tar.gz")
    with tarfile.open(bundle, mode="r:gz") as archive:
        members = {member.name.removeprefix("./"): member for member in archive.getmembers()}
        pkg_member = members.get("scratchbird/PKGBUILD")
        install_member = members.get("scratchbird/scratchbird.install")
        if pkg_member is None or install_member is None:
            fail("aur_lifecycle_files_missing")
        pkg_handle = archive.extractfile(pkg_member)
        install_handle = archive.extractfile(install_member)
        if pkg_handle is None or install_handle is None:
            fail("aur_lifecycle_files_unreadable")
        pkgbuild = pkg_handle.read().decode("utf-8")
        install = install_handle.read().decode("utf-8")
        source_members = [
            member
            for name, member in members.items()
            if name.startswith("scratchbird/scratchbird-")
            and name.endswith(".tar.gz")
            and member.isfile()
        ]
        if len(source_members) != 1:
            fail(f"aur_source_archive_count:{len(source_members)}")
        source_handle = archive.extractfile(source_members[0])
        if source_handle is None:
            fail("aur_source_archive_unreadable")
        source_bytes = source_handle.read()
    required_pkgbuild = (
        "install=scratchbird.install",
        "backup=(",
        "cp -a \"$srcdir\"/scratchbird-",
        "/usr \"$pkgdir\"/",
        "'systemd'",
        "'shadow'",
    )
    for fragment in required_pkgbuild:
        if fragment not in pkgbuild:
            fail(f"aur_pkgbuild_contract_missing:{fragment}")
    for fragment in (
        "post_install()",
        "post_upgrade()",
        "pre_remove()",
        "scratchbird-system-install post-install",
        "scratchbird-system-install pre-remove",
    ):
        if fragment not in install:
            fail(f"aur_install_contract_missing:{fragment}")
    for required_restore in ("config-preserve", "cp -an", "LINUX_SYSTEM_UNINSTALL_STATE.json"):
        if required_restore not in install:
            fail(f"aur_config_preservation_missing:{required_restore}")
    post_install = install.split("post_upgrade()", 1)[0]
    for forbidden in ("systemctl start", "systemctl enable", "systemctl preset", "--now"):
        if forbidden in post_install:
            fail(f"aur_post_install_service_activation_forbidden:{forbidden}")

    source_archive, source_index = tar_members(source_bytes)
    try:
        source_names = set(source_index)
        for required in SYSTEM_FILES:
            suffix = "/" + required
            if not any(name.endswith(suffix) for name in source_names):
                fail(f"aur_source_system_file_missing:{required}")
        check_no_database_or_security_sidecars(source_names, "aur_source")
    finally:
        source_archive.close()
    return {"bundle": bundle.name, "lifecycle": "present"}


def redact_secret(value: str, secret: str) -> str:
    return value.replace(secret, "<redacted>")


def redact_secrets(value: str, secrets_to_redact: tuple[str, ...]) -> str:
    """Remove every supplied secret, longest first, from retained diagnostics."""

    for secret in sorted(set(secrets_to_redact), key=len, reverse=True):
        if secret:
            value = redact_secret(value, secret)
    return value


def write_command_evidence(
    proof_root: Path,
    name: str,
    completed: subprocess.CompletedProcess[str],
    secrets_to_redact: tuple[str, ...],
) -> None:
    """Preserve command diagnostics without writing the bootstrap password."""

    (proof_root / f"{name}.stdout.txt").write_text(
        redact_secrets(completed.stdout, secrets_to_redact), encoding="utf-8"
    )
    (proof_root / f"{name}.stderr.txt").write_text(
        redact_secrets(completed.stderr, secrets_to_redact), encoding="utf-8"
    )
    (proof_root / f"{name}.status.txt").write_text(
        f"exit_code={completed.returncode}\n", encoding="utf-8"
    )


def run_secret_stdin_command(
    command: list[str],
    password: str,
    proof_root: Path,
    name: str,
    *,
    extra_redactions: tuple[str, ...] = (),
) -> subprocess.CompletedProcess[str]:
    """Run a CLI command with password material only on its standard input."""

    completed = subprocess.run(
        command,
        input=password + "\n",
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        timeout=120,
    )
    write_command_evidence(
        proof_root,
        name,
        completed,
        (password, *extra_redactions),
    )
    return completed


def privileged_path_test(prefix: list[str], flag: str, path: Path) -> bool:
    return subprocess.run(
        [*prefix, "test", flag, str(path)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    ).returncode == 0


def privileged_path_metadata(
    prefix: list[str],
    path: Path,
) -> tuple[int, int, int] | None:
    result = subprocess.run(
        [*prefix, "stat", "-c", "%u:%g:%a", "--", str(path)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    if result.returncode != 0:
        return None
    fields = result.stdout.strip().split(":")
    if len(fields) != 3:
        return None
    try:
        return (int(fields[0]), int(fields[1]), int(fields[2], 8))
    except ValueError:
        return None


def privileged_bootstrap_test_artifacts(
    prefix: list[str],
    database: Path,
) -> tuple[bool, list[Path]]:
    """List only files bearing the deterministic test database name."""

    listed = subprocess.run(
        [
            *prefix,
            "find",
            str(database.parent),
            "-maxdepth",
            "1",
            "-mindepth",
            "1",
            "-name",
            f"{database.name}*",
            "-print",
        ],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return (
        listed.returncode == 0,
        [Path(line) for line in listed.stdout.splitlines() if line],
    )


def privileged_database_or_security_artifacts(
    prefix: list[str],
) -> tuple[bool, list[str]]:
    """Inspect protected package data with the same authority as installation."""

    listed = subprocess.run(
        [
            *prefix,
            "find",
            "/var/lib/scratchbird",
            "-xdev",
            "-type",
            "f",
            "(",
            "-name",
            "*.sbdb",
            "-o",
            "-name",
            "*.sbrd",
            "-o",
            "-name",
            "*.security_principal_events*",
            "-o",
            "-name",
            "*.local_password_auth*",
            ")",
            "-print",
        ],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return listed.returncode == 0, [line for line in listed.stdout.splitlines() if line]


def bootstrap_command(prefix: list[str], tool: Path, database: Path) -> list[str]:
    return [
        *prefix,
        str(tool),
        "bootstrap",
        BOOTSTRAP_PRINCIPAL,
        str(database),
        "--mode=embedded",
        f"--platform-profile={BOOTSTRAP_PROFILE_PATH}",
        f"--resource-seed-pack-root={BOOTSTRAP_RESOURCE_PACK_ROOT}",
        f"--policy-seed-pack-root={BOOTSTRAP_POLICY_PACK_ROOT}",
        "--password-stdin",
    ]


def require_installed_bootstrap_inputs(prefix: list[str]) -> None:
    for name, path in BOOTSTRAP_TOOL_PATHS.items():
        if not privileged_path_test(prefix, "-f", path) or not privileged_path_test(
            prefix, "-x", path
        ):
            fail(f"privileged_bootstrap_tool_unavailable:{name}:{path}")
    if not privileged_path_test(prefix, "-f", BOOTSTRAP_PROFILE_PATH):
        fail(f"privileged_bootstrap_profile_unavailable:{BOOTSTRAP_PROFILE_PATH}")
    for label, path in (
        ("resource_seed_pack", BOOTSTRAP_RESOURCE_PACK_ROOT),
        ("policy_seed_pack", BOOTSTRAP_POLICY_PACK_ROOT),
        ("data_root", BOOTSTRAP_DATA_ROOT),
    ):
        if not privileged_path_test(prefix, "-d", path):
            fail(f"privileged_bootstrap_{label}_unavailable:{path}")


def remove_bootstrap_test_artifacts(
    prefix: list[str],
    database: Path,
    proof_root: Path,
) -> bool:
    """Remove test-owned data before package removal without touching data root."""

    listed, artifacts = privileged_bootstrap_test_artifacts(prefix, database)
    if not listed:
        return False
    if not artifacts:
        return True
    if any(privileged_path_test(prefix, "-d", path) for path in artifacts):
        return False
    removed = subprocess.run(
        [*prefix, "rm", "-f", "--", *(str(path) for path in artifacts)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    (proof_root / "bootstrap-cleanup.stdout.txt").write_text(
        removed.stdout, encoding="utf-8"
    )
    (proof_root / "bootstrap-cleanup.stderr.txt").write_text(
        removed.stderr, encoding="utf-8"
    )
    listed_after, artifacts_after = privileged_bootstrap_test_artifacts(prefix, database)
    return removed.returncode == 0 and listed_after and not artifacts_after


def service_identity_prefix(prefix: list[str]) -> list[str]:
    """Run the ordinary embedded client as its post-bootstrap file owner."""

    if prefix:
        return [*prefix, "-u", "scratchbird"]
    if shutil.which("runuser") is not None:
        return ["runuser", "-u", "scratchbird", "--"]
    fail("privileged_embedded_auth_service_runner_unavailable")


def assert_no_bootstrap_security_sidecars(prefix: list[str], database: Path) -> None:
    listed, artifacts = privileged_bootstrap_test_artifacts(prefix, database)
    if not listed:
        fail("privileged_bootstrap_security_sidecar_inventory_unavailable")
    found = [
        str(path)
        for path in artifacts
        if any(marker in path.name.lower() for marker in SECURITY_SIDECAR_MARKERS)
    ]
    if found:
        fail("privileged_bootstrap_security_sidecar_created:" + ",".join(found))


def assert_bootstrap_database_service_identity(
    prefix: list[str],
    database: Path,
    service_uid: int,
    service_gid: int,
) -> None:
    """Require bootstrap to leave the database owned by the locked service ID."""

    metadata = privileged_path_metadata(prefix, database)
    if metadata is None:
        fail("privileged_sbsql_bootstrap_database_metadata_unavailable")
    database_uid, database_gid, _ = metadata
    if database_uid != service_uid or database_gid != service_gid:
        fail(
            "privileged_sbsql_bootstrap_database_identity_mismatch:"
            f"expected={service_uid}:{service_gid}:actual={database_uid}:{database_gid}"
        )


def explicit_first_principal_bootstrap_smoke(
    prefix: list[str],
    proof_root: Path,
    service_uid: int,
    service_gid: int,
) -> dict[str, Any]:
    """Exercise the installed explicit bootstrap, then a normal local login.

    This intentionally occurs *after* the package lifecycle has proven it did
    not create a database.  The password is random per run, never placed in an
    argument or proof file, and is supplied only through standard input.
    """

    require_installed_bootstrap_inputs(prefix)
    database = BOOTSTRAP_DATA_ROOT / BOOTSTRAP_DATABASE_NAME
    listed, existing = privileged_bootstrap_test_artifacts(prefix, database)
    if not listed:
        fail("privileged_bootstrap_test_database_inventory_unavailable")
    if existing:
        fail(f"privileged_bootstrap_test_database_already_present:{database}")

    password = "InstallerBootstrap-" + secrets.token_urlsafe(24)
    created = run_secret_stdin_command(
        bootstrap_command(prefix, BOOTSTRAP_TOOL_PATHS["sbsql"], database),
        password,
        proof_root,
        "sbsql-first-principal-create",
    )
    if created.returncode != 0:
        fail(f"privileged_sbsql_bootstrap_failed:{created.returncode}")
    if not privileged_path_test(prefix, "-f", database):
        fail("privileged_sbsql_bootstrap_database_missing")
    assert_bootstrap_database_service_identity(
        prefix,
        database,
        service_uid,
        service_gid,
    )
    assert_no_bootstrap_security_sidecars(prefix, database)

    repeated_sbsql = run_secret_stdin_command(
        bootstrap_command(prefix, BOOTSTRAP_TOOL_PATHS["sbsql"], database),
        password,
        proof_root,
        "sbsql-repeat-create",
    )
    sbsql_repeat_text = repeated_sbsql.stdout + repeated_sbsql.stderr
    if (repeated_sbsql.returncode == 0 or
            "BOOTSTRAP.AUTH_DB_ALREADY_OWNED" not in sbsql_repeat_text):
        fail("privileged_sbsql_repeat_create_not_refused")

    repeated_sbsec = run_secret_stdin_command(
        bootstrap_command(prefix, BOOTSTRAP_TOOL_PATHS["sbsec"], database),
        password,
        proof_root,
        "sbsec-shared-repeat-create",
    )
    sbsec_repeat_text = repeated_sbsec.stdout + repeated_sbsec.stderr
    if (repeated_sbsec.returncode == 0 or
            "BOOTSTRAP.AUTH_DB_ALREADY_OWNED" not in sbsec_repeat_text):
        fail("privileged_sbsec_shared_repeat_create_not_refused")

    normal_prefix = service_identity_prefix(prefix)
    embedded_auth_command = [
        *normal_prefix,
        str(BOOTSTRAP_TOOL_PATHS["sbsql"]),
        str(database),
        "--mode=embedded",
        "--sslmode=disable",
        "-U",
        BOOTSTRAP_PRINCIPAL,
        "-q",
        "-A",
        "-t",
        "-c",
        "SELECT 1",
    ]
    denied = run_secret_stdin_command(
        embedded_auth_command,
        password + "-wrong",
        proof_root,
        "embedded-password-refusal",
        extra_redactions=(password,),
    )
    if denied.returncode == 0:
        fail("privileged_embedded_wrong_password_accepted")

    authenticated = run_secret_stdin_command(
        embedded_auth_command,
        password,
        proof_root,
        "embedded-password-authenticated-query",
    )
    if authenticated.returncode != 0:
        fail(f"privileged_embedded_password_auth_failed:{authenticated.returncode}")
    result_lines = [line.strip() for line in authenticated.stdout.splitlines() if line.strip()]
    if not result_lines or result_lines[-1] != "1":
        fail("privileged_embedded_password_auth_query_result_invalid")
    assert_no_bootstrap_security_sidecars(prefix, database)
    return {
        "status": "passed",
        "package_install_database_creation": "absent_before_explicit_bootstrap",
        "sbsql_first_principal_create": "passed",
        "database_owner_uid": service_uid,
        "database_owner_gid": service_gid,
        "security_sidecars": "absent",
        "sbsql_repeat_create": "BOOTSTRAP.AUTH_DB_ALREADY_OWNED",
        "sbsec_shared_repeat_create": "BOOTSTRAP.AUTH_DB_ALREADY_OWNED",
        "embedded_password_refusal": "passed",
        "embedded_password_authenticated_query": "SELECT 1",
    }


def privileged_install_skip(required: bool, reason: str) -> dict[str, Any]:
    """Return an optional-host skip, or fail a CI-required execution."""

    if required:
        fail(f"privileged_deb_install_required_but_skipped:{reason}")
    return {"status": "skipped", "reason": reason}


def privileged_install_smoke(
    deb: Path,
    requested: bool,
    work_root: Path,
    *,
    required: bool = False,
) -> dict[str, Any]:
    if required and not requested:
        fail("privileged_deb_install_required_without_request")
    if not requested:
        return {"status": "not_requested"}
    if os.environ.get("CI") != "true" and os.environ.get("SB_ALLOW_HOST_PACKAGE_MUTATION") != "1":
        return privileged_install_skip(required, "host_mutation_not_explicitly_allowed")
    if shutil.which("dpkg") is None or shutil.which("dpkg-query") is None:
        return privileged_install_skip(required, "dpkg_unavailable")
    prefix: list[str]
    if os.geteuid() == 0:
        prefix = []
    elif shutil.which("sudo") is not None:
        probe = subprocess.run(
            ["sudo", "-n", "true"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        if probe.returncode != 0:
            return privileged_install_skip(required, "noninteractive_root_unavailable")
        prefix = ["sudo", "-n"]
    else:
        return privileged_install_skip(required, "root_unavailable")

    preexisting = subprocess.run(
        ["dpkg-query", "-W", "-f=${Status}", "scratchbird"],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    if preexisting.returncode == 0:
        return privileged_install_skip(required, "scratchbird_package_already_present")

    install = subprocess.run(
        [*prefix, "dpkg", "-i", str(deb)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if install.returncode != 0:
        subprocess.run(
            [*prefix, "dpkg", "--remove", "scratchbird"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        fail(f"privileged_deb_install_failed:{install.returncode}:{install.stdout[-2000:]}")
    database = BOOTSTRAP_DATA_ROOT / BOOTSTRAP_DATABASE_NAME
    bootstrap_proof_root = work_root / "privileged-first-principal-bootstrap"
    bootstrap_proof_root.mkdir(parents=True, exist_ok=True)
    bootstrap_cleanup_ok = True
    try:
        passwd = subprocess.run(
            ["getent", "passwd", "scratchbird"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        group = subprocess.run(
            ["getent", "group", "scratchbird"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if passwd.returncode != 0 or group.returncode != 0:
            fail("privileged_identity_missing")
        if not passwd.stdout.rstrip().endswith(("/nologin", "/false")):
            fail("privileged_identity_not_non_login")
        passwd_fields = passwd.stdout.strip().split(":")
        group_fields = group.stdout.strip().split(":")
        if len(passwd_fields) != 7 or len(group_fields) < 3:
            fail("privileged_identity_record_malformed")
        service_uid = int(passwd_fields[2])
        service_gid = int(passwd_fields[3])
        if service_uid == 0 or service_gid == 0 or int(group_fields[2]) != service_gid:
            fail("privileged_identity_has_root_or_mismatched_group")
        if passwd_fields[5] != "/var/lib/scratchbird":
            fail("privileged_identity_home_mismatch")
        memberships = subprocess.run(
            ["id", "-nG", "scratchbird"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if memberships.returncode != 0 or set(memberships.stdout.split()) != {"scratchbird"}:
            fail("privileged_identity_has_unexpected_group_authority")
        for path in (
            Path("/var/lib/scratchbird/data"),
            Path("/var/log/scratchbird"),
            Path("/run/scratchbird/control"),
            Path("/run/scratchbird/runtime"),
        ):
            metadata = privileged_path_metadata(prefix, path)
            if metadata is None or metadata[2] != 0o750:
                fail(f"privileged_directory_contract_failed:{path}")
            if metadata[0] != service_uid or metadata[1] != service_gid:
                fail(f"privileged_directory_identity_mismatch:{path}")
        inventory_ok, installed_artifacts = privileged_database_or_security_artifacts(prefix)
        if not inventory_ok:
            fail("privileged_install_database_inventory_unavailable")
        if installed_artifacts:
            fail("privileged_install_created_database")
        bootstrap_proof = explicit_first_principal_bootstrap_smoke(
            prefix,
            bootstrap_proof_root,
            service_uid,
            service_gid,
        )
        if shutil.which("systemctl"):
            active = subprocess.run(
                ["systemctl", "is-active", "scratchbird-sbsrv.service"],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                check=False,
            ).stdout.strip()
            enabled = subprocess.run(
                ["systemctl", "is-enabled", "scratchbird-sbsrv.service"],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                check=False,
            ).stdout.strip()
            if active == "active" or enabled == "enabled":
                fail(f"privileged_service_activated:{enabled}:{active}")
    finally:
        bootstrap_cleanup_ok = remove_bootstrap_test_artifacts(
            prefix,
            database,
            bootstrap_proof_root,
        )
        remove = subprocess.run(
            [*prefix, "dpkg", "--remove", "scratchbird"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        if remove.returncode != 0:
            fail(f"privileged_deb_remove_failed:{remove.returncode}:{remove.stdout[-2000:]}")
        if not bootstrap_cleanup_ok:
            fail("privileged_bootstrap_test_cleanup_failed")
    if not privileged_path_test(prefix, "-d", Path("/var/lib/scratchbird")):
        fail("privileged_uninstall_removed_user_data_root")
    for config in CONFIG_FILES:
        if not privileged_path_test(prefix, "-f", Path("/", config)):
            fail(f"privileged_uninstall_removed_configuration:{config}")
    if privileged_path_test(
        prefix,
        "-e",
        Path("/usr/lib/systemd/system/scratchbird-sbsrv.service"),
    ):
        fail("privileged_uninstall_retained_service_unit")
    return {
        "status": "passed",
        "uninstall_preserved_data": True,
        "uninstall_preserved_configuration": True,
        "uninstall_removed_service_unit": True,
        "first_principal_bootstrap": bootstrap_proof,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifact-root", type=Path, required=True)
    parser.add_argument("--work-root", type=Path, required=True)
    parser.add_argument("--run-privileged-deb-install", action="store_true")
    parser.add_argument(
        "--require-privileged-deb-install",
        action="store_true",
        help=(
            "fail instead of reporting a skipped privileged DEB install; "
            "for disposable CI hosts"
        ),
    )
    args = parser.parse_args()

    root = args.artifact_root.resolve()
    work_root = args.work_root.resolve()
    if work_root.exists():
        shutil.rmtree(work_root)
    work_root.mkdir(parents=True)

    portable = exactly_one(root, "scratchbird-linux-*.tar.gz")
    deb = exactly_one(root, "scratchbird_*.deb")
    proof: dict[str, Any] = {
        "schema_id": "scratchbird.linux_system_package_smoke.v1",
        "portable": verify_portable_tar(portable),
        "package_evidence": verify_package_evidence(root),
    }
    proof["deb"], _ = verify_deb(deb, work_root)
    proof["rpm_recipe"] = verify_rpm_recipe(root)
    proof["aur"] = verify_aur(root)
    proof["privileged_install"] = privileged_install_smoke(
        deb,
        args.run_privileged_deb_install,
        work_root,
        required=args.require_privileged_deb_install,
    )
    proof["result"] = "passed"
    proof_path = work_root / "LINUX_SYSTEM_PACKAGE_SMOKE.json"
    proof_path.write_text(json.dumps(proof, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"smoke_install_linux_system=passed:{proof_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
