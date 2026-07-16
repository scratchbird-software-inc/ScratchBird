#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

"""Prepare a user-owned, TLS-enabled native SBSQL QA instance."""

from __future__ import annotations

import argparse
import ctypes
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import stat
import subprocess
import sys

if os.name != "nt":
    import grp
    import pwd
else:  # pragma: no cover - POSIX identity modules do not exist on Windows.
    grp = None
    pwd = None


CONFIG_NAMES = (
    "SBsrv.conf",
    "SBgate.conf",
    "SBmgr.conf",
    "SBParser.conf",
    "SBbootstrap.profile",
)
NATIVE_BINARIES = ("SBsrv", "SBgate", "SBmgr", "SBParser", "SBsec", "SBsql")
RESOURCE_PACK = Path(
    "share/scratchbird/resources/seed-packs/initial-resource-pack"
)
POLICY_PACK = Path(
    "share/scratchbird/resources/policy-packs/default-local-password"
)


def fail(message: str) -> None:
    print(f"prepare_native_qa_instance=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def safe_value(path: Path) -> str:
    value = path.resolve().as_posix()
    if "\n" in value or "\r" in value or "#" in value:
        fail("path_contains_unsafe_config_character")
    return value


def profile_platform() -> str:
    if os.name == "nt":
        return "windows"
    if sys.platform == "darwin":
        return "macos"
    if sys.platform.startswith("linux"):
        return "linux"
    return "posix"


WINDOWS_SERVICE_IDENTITY = r"NT SERVICE\scratchbird"
WINDOWS_SERVICE_GROUP = "ScratchBird"
POSIX_SERVICE_IDENTITY = "scratchbird"
POSIX_SERVICE_GROUP = "scratchbird"
MACOS_IMPLICIT_BASELINE_GIDS = frozenset({12, 61})


def require_posix_service_authority() -> tuple[str, str]:
    if os.name == "nt" or pwd is None or grp is None:
        fail("posix_service_identity_platform_invalid")
    if not hasattr(os, "geteuid") or not hasattr(os, "getegid"):
        fail("posix_service_numeric_identity_unavailable")
    effective_uid = os.geteuid()
    effective_gid = os.getegid()
    if effective_uid == 0 or effective_gid == 0:
        fail("prepare_instance_as_service_identity_not_root")
    try:
        user_by_uid = pwd.getpwuid(effective_uid)
        user_by_name = pwd.getpwnam(POSIX_SERVICE_IDENTITY)
        group_by_gid = grp.getgrgid(effective_gid)
        group_by_name = grp.getgrnam(POSIX_SERVICE_GROUP)
        all_users = list(pwd.getpwall())
        all_groups = list(grp.getgrall())
    except (KeyError, OSError):
        fail("posix_service_local_identity_required")

    if (
        user_by_uid.pw_name != POSIX_SERVICE_IDENTITY
        or user_by_name.pw_name != POSIX_SERVICE_IDENTITY
        or user_by_uid.pw_uid != effective_uid
        or user_by_name.pw_uid != effective_uid
        or user_by_uid.pw_gid != effective_gid
        or user_by_name.pw_gid != effective_gid
    ):
        fail("posix_service_local_user_required")
    if (
        group_by_gid.gr_name != POSIX_SERVICE_GROUP
        or group_by_name.gr_name != POSIX_SERVICE_GROUP
        or group_by_gid.gr_gid != effective_gid
        or group_by_name.gr_gid != effective_gid
    ):
        fail("posix_service_local_group_required")

    named_users = [
        row for row in all_users if row.pw_name == POSIX_SERVICE_IDENTITY
    ]
    numeric_users = [row for row in all_users if row.pw_uid == effective_uid]
    named_groups = [
        row for row in all_groups if row.gr_name == POSIX_SERVICE_GROUP
    ]
    numeric_groups = [row for row in all_groups if row.gr_gid == effective_gid]
    if (
        len(named_users) != 1
        or len(numeric_users) != 1
        or len(named_groups) != 1
        or len(numeric_groups) != 1
    ):
        fail("posix_service_local_identity_not_unique")

    current_groups = {effective_gid, *os.getgroups()}
    recorded_groups = {
        row.gr_gid
        for row in all_groups
        if POSIX_SERVICE_IDENTITY in row.gr_mem
    }
    recorded_groups.add(user_by_uid.pw_gid)
    allowed_current_groups = {effective_gid}
    if sys.platform == "darwin":
        allowed_current_groups.update(MACOS_IMPLICIT_BASELINE_GIDS)
    if (
        effective_gid not in current_groups
        or not current_groups.issubset(allowed_current_groups)
        or recorded_groups != {effective_gid}
    ):
        fail("posix_service_numeric_membership_not_exact")
    return POSIX_SERVICE_IDENTITY, POSIX_SERVICE_GROUP


def same_path(left: Path, right: Path) -> bool:
    return os.path.normcase(str(left.resolve())) == os.path.normcase(
        str(right.resolve())
    )


def require_windows_system_service_authority(
    install_root: Path, instance_root: Path
) -> None:
    if os.name != "nt":
        fail("windows_system_service_mode_requires_windows")
    if not ctypes.windll.shell32.IsUserAnAdmin():
        fail("windows_system_service_administrator_required")
    advapi = ctypes.WinDLL("advapi32", use_last_error=True)
    advapi.LookupAccountNameW.argtypes = [
        ctypes.c_wchar_p,
        ctypes.c_wchar_p,
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_uint32),
        ctypes.c_wchar_p,
        ctypes.POINTER(ctypes.c_uint32),
        ctypes.POINTER(ctypes.c_uint32),
    ]
    advapi.LookupAccountNameW.restype = ctypes.c_int
    sid_size = ctypes.c_uint32()
    domain_size = ctypes.c_uint32()
    sid_use = ctypes.c_uint32()
    account = f"{os.environ.get('COMPUTERNAME', '')}\\{WINDOWS_SERVICE_GROUP}"
    advapi.LookupAccountNameW(
        None,
        account,
        None,
        ctypes.byref(sid_size),
        None,
        ctypes.byref(domain_size),
        ctypes.byref(sid_use),
    )
    if sid_size.value == 0 or domain_size.value == 0:
        fail("windows_local_scratchbird_group_required")
    sid = ctypes.create_string_buffer(sid_size.value)
    domain = ctypes.create_unicode_buffer(domain_size.value)
    if not advapi.LookupAccountNameW(
        None,
        account,
        sid,
        ctypes.byref(sid_size),
        domain,
        ctypes.byref(domain_size),
        ctypes.byref(sid_use),
    ):
        fail("windows_local_scratchbird_group_required")
    if (
        sid_use.value != 4
        or domain.value.casefold()
        != os.environ.get("COMPUTERNAME", "").casefold()
    ):
        fail("windows_local_scratchbird_group_required")
    program_files = os.environ.get("ProgramFiles")
    program_data = os.environ.get("ProgramData")
    if not program_files or not program_data:
        fail("windows_system_known_folders_unavailable")
    if not same_path(
        install_root, Path(program_files) / "ScratchBird"
    ) or not same_path(
        instance_root, Path(program_data) / "ScratchBird"
    ):
        fail("windows_system_service_canonical_roots_required")

    import winreg

    try:
        with winreg.OpenKey(
            winreg.HKEY_LOCAL_MACHINE,
            r"SYSTEM\CurrentControlSet\Services\scratchbird",
            0,
            winreg.KEY_READ | winreg.KEY_WOW64_64KEY,
        ) as service_key:
            image_path = str(winreg.QueryValueEx(service_key, "ImagePath")[0])
            object_name = str(winreg.QueryValueEx(service_key, "ObjectName")[0])
            start_type = int(winreg.QueryValueEx(service_key, "Start")[0])
            sid_type = int(winreg.QueryValueEx(service_key, "ServiceSidType")[0])
    except OSError:
        fail("windows_system_service_registration_invalid")
    expected_binary = str((install_root / "bin" / "SBsrv.exe").resolve())
    expected_config = str(
        (instance_root / "config" / "SBsrv.conf").resolve()
    )
    folded_image = image_path.casefold()
    if (
        expected_binary.casefold() not in folded_image
        or expected_config.casefold() not in folded_image
        or "--service" not in folded_image
        or object_name.casefold() != WINDOWS_SERVICE_IDENTITY.casefold()
        or start_type != 3
        or sid_type != 3
    ):
        fail("windows_system_service_registration_invalid")
    advapi.OpenSCManagerW.argtypes = [
        ctypes.c_wchar_p,
        ctypes.c_wchar_p,
        ctypes.c_uint32,
    ]
    advapi.OpenSCManagerW.restype = ctypes.c_void_p
    advapi.OpenServiceW.argtypes = [
        ctypes.c_void_p,
        ctypes.c_wchar_p,
        ctypes.c_uint32,
    ]
    advapi.OpenServiceW.restype = ctypes.c_void_p
    advapi.QueryServiceStatusEx.argtypes = [
        ctypes.c_void_p,
        ctypes.c_int,
        ctypes.c_void_p,
        ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_uint32),
    ]
    advapi.QueryServiceStatusEx.restype = ctypes.c_int
    advapi.CloseServiceHandle.argtypes = [ctypes.c_void_p]
    advapi.CloseServiceHandle.restype = ctypes.c_int
    manager = advapi.OpenSCManagerW(None, None, 0x0001)
    if not manager:
        fail("windows_system_service_registration_invalid")
    service = advapi.OpenServiceW(manager, "scratchbird", 0x0004)
    if not service:
        advapi.CloseServiceHandle(manager)
        fail("windows_system_service_registration_invalid")

    class ServiceStatusProcess(ctypes.Structure):
        _fields_ = [
            ("service_type", ctypes.c_uint32),
            ("current_state", ctypes.c_uint32),
            ("controls_accepted", ctypes.c_uint32),
            ("win32_exit_code", ctypes.c_uint32),
            ("service_specific_exit_code", ctypes.c_uint32),
            ("check_point", ctypes.c_uint32),
            ("wait_hint", ctypes.c_uint32),
            ("process_id", ctypes.c_uint32),
            ("service_flags", ctypes.c_uint32),
        ]

    status = ServiceStatusProcess()
    returned = ctypes.c_uint32()
    queried = advapi.QueryServiceStatusEx(
        service,
        0,
        ctypes.byref(status),
        ctypes.sizeof(status),
        ctypes.byref(returned),
    )
    advapi.CloseServiceHandle(service)
    advapi.CloseServiceHandle(manager)
    if not queried or status.current_state != 1:
        fail("windows_system_service_must_be_stopped")


def materialize_windows_system_default(
    text: str, install_root: Path, instance_root: Path
) -> str:
    rendered = text.replace(
        "@SCRATCHBIRD_INSTALL_ROOT@", install_root.resolve().as_posix()
    ).replace(
        "@SCRATCHBIRD_STATE_ROOT@", instance_root.resolve().as_posix()
    )
    if "@SCRATCHBIRD_" in rendered:
        fail("windows_system_config_template_invalid")
    return rendered


def prepare_windows_system_config_root(
    install_root: Path,
    instance_root: Path,
    defaults_root: Path,
) -> Path:
    live_root = instance_root / "config"
    live_root.mkdir(parents=True, exist_ok=True)
    expected: dict[str, str] = {}
    for name in CONFIG_NAMES:
        source = defaults_root / name
        if not source.is_file():
            fail("windows_system_config_defaults_missing")
        expected[name] = materialize_windows_system_default(
            source.read_text(encoding="utf-8"), install_root, instance_root
        )

    for name, content in expected.items():
        destination = live_root / name
        if destination.exists() and (
            not destination.is_file()
            or destination.read_text(encoding="utf-8") != content
        ):
            fail("windows_system_existing_config_mismatch")

    database = instance_root / "data" / "default.sbdb"
    forbidden_state = (
        database,
        instance_root / "tls" / "server-cert.pem",
        instance_root / "tls" / "server-key.pem",
        instance_root / "secrets" / "listener-dbbt-key.hex",
        instance_root / "install" / "NATIVE_QA_INSTANCE.json",
    )
    if any(path.exists() for path in forbidden_state):
        fail("windows_system_qa_state_already_present")
    for name, content in expected.items():
        destination = live_root / name
        if destination.exists():
            continue
        temporary = destination.with_name(destination.name + ".tmp")
        temporary.write_text(content, encoding="utf-8")
        os.replace(temporary, destination)
    return live_root.resolve()


def configure_windows_service_dbbt(dbbt_key: Path) -> str:
    if os.name != "nt":
        return "shell_environment_required"
    import winreg

    encoded = dbbt_key.read_text(encoding="ascii").strip()
    if not re.fullmatch(r"[0-9a-f]{64}", encoded):
        fail("windows_system_dbbt_key_invalid")
    try:
        with winreg.OpenKey(
            winreg.HKEY_LOCAL_MACHINE,
            r"SYSTEM\CurrentControlSet\Services\scratchbird",
            0,
            winreg.KEY_READ | winreg.KEY_SET_VALUE | winreg.KEY_WOW64_64KEY,
        ) as service_key:
            try:
                current, kind = winreg.QueryValueEx(service_key, "Environment")
            except FileNotFoundError:
                current, kind = [], winreg.REG_MULTI_SZ
            if kind != winreg.REG_MULTI_SZ:
                fail("windows_system_service_environment_invalid")
            retained = [
                row
                for row in current
                if not row.startswith("SCRATCHBIRD_LISTENER_DBBT_KEY_HEX=")
            ]
            if len(retained) != len(current):
                fail("windows_system_service_environment_already_configured")
            retained.append(f"SCRATCHBIRD_LISTENER_DBBT_KEY_HEX={encoded}")
            winreg.SetValueEx(
                service_key, "Environment", 0, winreg.REG_MULTI_SZ, retained
            )
    except OSError:
        fail("windows_system_service_environment_write_failed")
    return "protected_service_registry_environment"


def safe_profile_token(value: str, label: str) -> str:
    if not re.fullmatch(r"[A-Za-z0-9_.-]{1,255}", value):
        fail(f"{label}_invalid")
    if value == "operator_required":
        fail(f"{label}_unconfigured")
    return value


def validate_service_authority(
    platform: str, service_identity: str, service_group: str
) -> tuple[str, str]:
    if platform == "windows":
        if service_identity != WINDOWS_SERVICE_IDENTITY:
            fail("windows_managed_service_identity_required")
        if service_group != WINDOWS_SERVICE_GROUP:
            fail("windows_local_scratchbird_group_required")
        return service_identity, service_group
    if platform not in {"linux", "macos", "posix"}:
        fail("service_platform_invalid")
    safe_profile_token(service_identity, "service_identity")
    safe_profile_token(service_group, "service_group")
    if service_identity != POSIX_SERVICE_IDENTITY:
        fail("posix_scratchbird_service_identity_required")
    if service_group != POSIX_SERVICE_GROUP:
        fail("posix_scratchbird_service_group_required")
    return service_identity, service_group


def handoff_windows_instance_acl(
    instance_root: Path, service_identity: str
) -> str:
    if os.name != "nt":
        return "drop_to_service_identity_before_create"
    if service_identity != WINDOWS_SERVICE_IDENTITY:
        fail("windows_managed_service_identity_required")
    icacls = shutil.which("icacls.exe") or shutil.which("icacls")
    if icacls is None:
        fail("windows_icacls_not_found")
    result = subprocess.run(
        [
            icacls,
            str(instance_root),
            "/grant:r",
            f"{WINDOWS_SERVICE_IDENTITY}:(OI)(CI)F",
            "/T",
            "/Q",
        ],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        fail("windows_service_identity_acl_handoff_failed")
    return "explicit_managed_service_sid_acl"


def executable(install_root: Path, name: str) -> Path:
    suffix = ".exe" if os.name == "nt" else ""
    path = install_root / "bin" / f"{name}{suffix}"
    if not path.is_file():
        fail(f"native_binary_missing:{name}:{path}")
    return path.resolve()


def discover_config_root(install_root: Path, explicit: Path | None) -> Path:
    candidates = []
    if explicit is not None:
        candidates.append(explicit)
    candidates.extend(
        (
            install_root / "share" / "scratchbird" / "config-defaults",
            install_root / "etc" / "scratchbird",
            install_root.parent.parent / "etc" / "scratchbird",
            Path("/etc/scratchbird"),
        )
    )
    for candidate in candidates:
        if all((candidate / name).is_file() for name in CONFIG_NAMES):
            return candidate.resolve()
    fail("config_root_not_found")


def require_empty_instance_root(instance_root: Path) -> None:
    if instance_root.exists() and any(instance_root.iterdir()):
        fail(f"instance_root_not_empty:{instance_root}")
    instance_root.mkdir(parents=True, exist_ok=True)
    if os.name != "nt":
        instance_root.chmod(0o700)


def make_private_directory(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True)
    if os.name != "nt":
        path.chmod(0o700)


def require_private_key(path: Path) -> Path:
    path = path.resolve()
    if not path.is_file():
        fail(f"tls_key_missing:{path}")
    if os.name != "nt" and stat.S_IMODE(path.stat().st_mode) & 0o077:
        fail(f"tls_key_permissions_not_private:{path}")
    return path


def supplied_tls(cert: Path | None, key: Path | None) -> tuple[Path, Path] | None:
    if cert is None and key is None:
        return None
    if cert is None or key is None:
        fail("tls_cert_and_key_must_be_supplied_together")
    cert = cert.resolve()
    if not cert.is_file():
        fail(f"tls_certificate_missing:{cert}")
    return cert, require_private_key(key)


def generate_tls(
    instance_root: Path,
    openssl_command: str,
    hostname: str,
    days: int,
) -> tuple[Path, Path]:
    if not re.fullmatch(r"[A-Za-z0-9.-]+", hostname):
        fail("tls_hostname_invalid")
    openssl = shutil.which(openssl_command)
    if openssl is None:
        fail(f"openssl_not_found:{openssl_command}")
    tls_root = instance_root / "tls"
    make_private_directory(tls_root)
    cert = tls_root / "server-cert.pem"
    key = tls_root / "server-key.pem"
    openssl_config = tls_root / "openssl-qa.cnf"
    dns_rows = [f"DNS.1 = {hostname}"]
    if hostname.lower() != "localhost":
        dns_rows.append("DNS.2 = localhost")
    openssl_config.write_text(
        "\n".join(
            (
                "[req]",
                "distinguished_name = dn",
                "x509_extensions = v3_req",
                "prompt = no",
                "",
                "[dn]",
                f"CN = {hostname}",
                "",
                "[v3_req]",
                "basicConstraints = critical,CA:FALSE",
                "keyUsage = critical,digitalSignature,keyEncipherment",
                "extendedKeyUsage = serverAuth",
                "subjectAltName = @alt_names",
                "",
                "[alt_names]",
                *dns_rows,
                "IP.1 = 127.0.0.1",
                "IP.2 = ::1",
                "",
            )
        ),
        encoding="utf-8",
    )
    previous_umask = os.umask(0o077) if os.name != "nt" else None
    try:
        result = subprocess.run(
            [
                openssl,
                "req",
                "-x509",
                "-newkey",
                "rsa:3072",
                "-sha256",
                "-nodes",
                "-days",
                str(days),
                "-config",
                str(openssl_config),
                "-keyout",
                str(key),
                "-out",
                str(cert),
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    finally:
        if previous_umask is not None:
            os.umask(previous_umask)
    if result.returncode != 0:
        fail(f"openssl_certificate_generation_failed:exit={result.returncode}")
    if os.name != "nt":
        key.chmod(0o600)
        cert.chmod(0o644)
    return cert.resolve(), require_private_key(key)


def generate_dbbt_key(instance_root: Path) -> Path:
    secret_root = instance_root / "secrets"
    make_private_directory(secret_root)
    path = secret_root / "listener-dbbt-key.hex"
    previous_umask = os.umask(0o077) if os.name != "nt" else None
    try:
        path.write_text(secrets.token_hex(32) + "\n", encoding="ascii")
    finally:
        if previous_umask is not None:
            os.umask(previous_umask)
    if os.name != "nt":
        path.chmod(0o600)
    return path.resolve()


def render_flat_config(text: str, replacements: dict[str, str]) -> str:
    found: set[str] = set()
    output: list[str] = []
    for raw in text.splitlines():
        candidate = raw.split("#", 1)[0].strip()
        if "=" in candidate:
            key = candidate.split("=", 1)[0].strip()
            if key in replacements:
                output.append(f"{key} = {replacements[key]}")
                found.add(key)
                continue
        output.append(raw)
    missing = [key for key in replacements if key not in found]
    if missing:
        output.extend(("", "# Native QA instance paths generated locally."))
        output.extend(f"{key} = {replacements[key]}" for key in missing)
    return "\n".join(output).rstrip() + "\n"


def render_server_config(text: str, replacements: dict[str, str]) -> str:
    section = ""
    found: set[str] = set()
    output: list[str] = []
    for raw in text.splitlines():
        candidate = raw.split("#", 1)[0].strip()
        if candidate.startswith("[") and candidate.endswith("]"):
            section = candidate[1:-1].strip()
        if "=" in candidate and section:
            key = candidate.split("=", 1)[0].strip()
            canonical = f"{section}.{key}"
            if canonical in replacements:
                output.append(f"{key} = {replacements[canonical]}")
                found.add(canonical)
                continue
        output.append(raw)

    missing_by_section: dict[str, list[tuple[str, str]]] = {}
    for canonical, value in replacements.items():
        if canonical in found:
            continue
        section_name, key = canonical.rsplit(".", 1)
        missing_by_section.setdefault(section_name, []).append((key, value))
    for section_name, rows in missing_by_section.items():
        output.extend(("", "# Native QA instance paths generated locally.", f"[{section_name}]"))
        output.extend(f"{key} = {value}" for key, value in rows)
    return "\n".join(output).rstrip() + "\n"


def write_configs(
    install_root: Path,
    config_root: Path,
    instance_root: Path,
    cert: Path,
    key: Path,
    service_identity: str,
    service_group: str,
    windows_system_service: bool = False,
) -> dict[str, Path]:
    config_output = instance_root / "config"
    make_private_directory(config_output)
    bin_paths = {name: executable(install_root, name) for name in NATIVE_BINARIES}
    resource_pack = (install_root / RESOURCE_PACK).resolve()
    policy_pack = (install_root / POLICY_PACK).resolve()
    if not resource_pack.is_dir():
        fail(f"resource_seed_pack_missing:{resource_pack}")
    if not policy_pack.is_dir():
        fail(f"policy_seed_pack_missing:{policy_pack}")

    runtime = (
        instance_root / "run" / "sb_server"
        if windows_system_service
        else instance_root / "runtime"
    ).resolve()
    listener_runtime = (
        instance_root / "run" / "listener"
        if windows_system_service
        else runtime / "listener"
    ).resolve()
    manager_runtime = (
        instance_root / "run" / "manager"
        if windows_system_service
        else runtime / "manager"
    ).resolve()
    log_root = (
        instance_root / "log"
        if windows_system_service
        else instance_root / "logs"
    ).resolve()
    database = (instance_root / "data" / "default.sbdb").resolve()
    directories = (
        runtime,
        runtime / "data",
        runtime / "control",
        listener_runtime,
        listener_runtime / "control",
        listener_runtime / "runtime",
        manager_runtime,
        manager_runtime / "control",
        manager_runtime / "runtime",
        runtime / "databases",
        database.parent,
        log_root,
    )
    for directory in directories:
        make_private_directory(directory)

    server_replacements = {
        "server.runtime.data_dir": safe_value(runtime / "data"),
        "server.runtime.control_dir": safe_value(runtime / "control"),
        "server.logging.log_file": safe_value(log_root / "SBsrv.log"),
        "server.database.default_path": safe_value(database),
        "server.database.resource_seed_pack_root": safe_value(resource_pack),
        "server.database.policy_seed_pack_root": safe_value(policy_pack),
        "server.listener.native.executable_path": safe_value(bin_paths["SBgate"]),
        "server.listener.native.parser_executable_path": safe_value(bin_paths["SBParser"]),
        "server.listener.native.control_dir": safe_value(listener_runtime / "control"),
        "server.listener.native.runtime_dir": safe_value(listener_runtime / "runtime"),
        "server.listener.native.tls_cert_file": safe_value(cert),
        "server.listener.native.tls_key_file": safe_value(key),
        "server.parser.sbps_endpoint": safe_value(runtime / "control" / "sb_server.sbps.sock"),
    }
    listener_replacements = {
        "parser_executable": safe_value(bin_paths["SBParser"]),
        "server_endpoint": safe_value(runtime / "control" / "sb_server.sbps.sock"),
        "control_dir": safe_value(listener_runtime / "control"),
        "runtime_dir": safe_value(listener_runtime / "runtime"),
        "tls_cert_file": safe_value(cert),
        "tls_key_file": safe_value(key),
    }
    manager_replacements = {
        "manager.runtime_dir": safe_value(manager_runtime / "runtime"),
        "manager.control_dir": safe_value(manager_runtime / "control"),
        "manager.log.path": safe_value(log_root / "SBmgr.log"),
        "manager.owner.database_path": safe_value(database),
        "manager.proxy.tls_cert_file": safe_value(cert),
        "manager.proxy.tls_key_file": safe_value(key),
    }
    parser_replacements = {
        "parser.worker_binary": safe_value(bin_paths["SBParser"]),
    }
    bootstrap_profile_replacements = {
        "platform": profile_platform(),
        "service_identity": service_identity,
        "service_group": service_group,
    }

    rendered = {
        "SBsrv.conf": render_server_config(
            (config_root / "SBsrv.conf").read_text(encoding="utf-8"),
            server_replacements,
        ),
        "SBgate.conf": render_flat_config(
            (config_root / "SBgate.conf").read_text(encoding="utf-8"),
            listener_replacements,
        ),
        "SBmgr.conf": render_flat_config(
            (config_root / "SBmgr.conf").read_text(encoding="utf-8"),
            manager_replacements,
        ),
        "SBParser.conf": render_flat_config(
            (config_root / "SBParser.conf").read_text(encoding="utf-8"),
            parser_replacements,
        ),
        "SBbootstrap.profile": render_flat_config(
            (config_root / "SBbootstrap.profile").read_text(encoding="utf-8"),
            bootstrap_profile_replacements,
        ),
    }
    paths: dict[str, Path] = {}
    for name, content in rendered.items():
        path = config_output / name
        temporary = path.with_name(path.name + ".tmp")
        temporary.write_text(content, encoding="utf-8")
        os.replace(temporary, path)
        if os.name != "nt":
            path.chmod(0o600)
        paths[name] = path.resolve()
    return paths


def validate_configs(
    install_root: Path,
    instance_root: Path,
    configs: dict[str, Path],
    resource_pack: Path,
    policy_pack: Path,
    windows_system_service: bool = False,
) -> list[dict[str, object]]:
    validation_root = (
        instance_root / "install" / "validation"
        if windows_system_service
        else instance_root / "validation"
    )
    make_private_directory(validation_root)
    commands = (
        ("SBsrv", ["--config", str(configs["SBsrv.conf"]), "--validate-config"]),
        ("SBgate", [f"--config={configs['SBgate.conf']}", "--validate-config"]),
        ("SBmgr", ["--config", str(configs["SBmgr.conf"]), "--validate-config"]),
    )
    environment = os.environ.copy()
    environment["SCRATCHBIRD_RESOURCE_SEED_PACK_ROOT"] = str(resource_pack)
    environment["SCRATCHBIRD_POLICY_SEED_PACK_ROOT"] = str(policy_pack)
    rows: list[dict[str, object]] = []
    for name, arguments in commands:
        binary = executable(install_root, name)
        result = subprocess.run(
            [str(binary), *arguments],
            cwd=install_root,
            env=environment,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        (validation_root / f"{name}.stdout.txt").write_text(
            result.stdout, encoding="utf-8"
        )
        (validation_root / f"{name}.stderr.txt").write_text(
            result.stderr, encoding="utf-8"
        )
        rows.append({"binary": name, "exit_code": result.returncode})
        if result.returncode != 0:
            fail(f"config_validation_failed:{name}:exit={result.returncode}")
    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--install-root", type=Path, required=True)
    parser.add_argument("--config-root", type=Path)
    parser.add_argument("--instance-root", type=Path, required=True)
    tls = parser.add_mutually_exclusive_group(required=True)
    tls.add_argument("--generate-self-signed-tls", action="store_true")
    tls.add_argument("--tls-cert", type=Path)
    parser.add_argument("--tls-key", type=Path)
    parser.add_argument("--openssl", default="openssl")
    parser.add_argument("--hostname", default="localhost")
    parser.add_argument("--tls-days", type=int, default=14)
    parser.add_argument("--skip-validation", action="store_true")
    parser.add_argument(
        "--windows-system-service",
        action="store_true",
        help=(
            "Explicitly prepare the installer-registered canonical "
            "ProgramData Windows service instance."
        ),
    )
    parser.add_argument("--service-identity")
    parser.add_argument("--service-group", required=True)
    args = parser.parse_args()

    if args.tls_days < 1 or args.tls_days > 397:
        fail("tls_days_out_of_range")
    if args.generate_self_signed_tls and args.tls_key is not None:
        fail("tls_key_cannot_accompany_generated_tls")
    if args.tls_cert is not None and args.tls_key is None:
        fail("tls_cert_and_key_must_be_supplied_together")
    platform = profile_platform()
    if args.windows_system_service and platform != "windows":
        fail("windows_system_service_mode_requires_windows")
    if platform == "windows" and args.service_identity is None:
        fail("windows_dedicated_service_identity_required")
    selected_service_identity = args.service_identity or POSIX_SERVICE_IDENTITY
    service_identity, service_group = validate_service_authority(
        platform, selected_service_identity, args.service_group
    )
    if os.name != "nt":
        require_posix_service_authority()

    install_root = args.install_root.resolve()
    if not install_root.is_dir():
        fail(f"install_root_not_found:{install_root}")
    for name in NATIVE_BINARIES:
        executable(install_root, name)
    instance_root = args.instance_root.resolve()
    if args.windows_system_service:
        require_windows_system_service_authority(install_root, instance_root)
        defaults_root = (
            install_root / "share" / "scratchbird" / "config-defaults"
        ).resolve()
        if args.config_root is not None and not same_path(
            args.config_root, defaults_root
        ):
            fail("windows_system_config_defaults_root_required")
        config_root = prepare_windows_system_config_root(
            install_root, instance_root, defaults_root
        )
    else:
        config_root = discover_config_root(install_root, args.config_root)
        require_empty_instance_root(instance_root)

    if args.generate_self_signed_tls:
        cert, key = generate_tls(
            instance_root, args.openssl, args.hostname, args.tls_days
        )
        tls_source = "generated_local_self_signed_qa_only"
    else:
        provided = supplied_tls(args.tls_cert, args.tls_key)
        if provided is None:
            fail("tls_material_required")
        cert, key = provided
        tls_source = "operator_supplied"

    configs = write_configs(
        install_root,
        config_root,
        instance_root,
        cert,
        key,
        service_identity,
        service_group,
        windows_system_service=args.windows_system_service,
    )
    dbbt_key = generate_dbbt_key(instance_root)
    resource_pack = (install_root / RESOURCE_PACK).resolve()
    policy_pack = (install_root / POLICY_PACK).resolve()
    validations = []
    if not args.skip_validation:
        validations = validate_configs(
            install_root,
            instance_root,
            configs,
            resource_pack,
            policy_pack,
            windows_system_service=args.windows_system_service,
        )
    dbbt_service_source = configure_windows_service_dbbt(dbbt_key) if (
        args.windows_system_service
    ) else "shell_environment_required"

    manifest = {
        "schema_id": "scratchbird.native_qa_instance.v1",
        "native_parser": "SBSQL",
        "emulation_components": "excluded",
        "install_root": str(install_root),
        "instance_root": str(instance_root),
        "windows_system_service": args.windows_system_service,
        "configuration": {name: str(path) for name, path in configs.items()},
        "resource_seed_pack_root": str(resource_pack),
        "policy_seed_pack_root": str(policy_pack),
        "bootstrap": {
            "required_before_server_start": True,
            "tool": str(executable(install_root, "SBsec")),
            "database_must_not_exist": True,
            "mode": "embedded",
            "credential_input": "protected_prompt_or_stdin",
            "native_network_port_after_start": 3092,
            "platform_profile": str(configs["SBbootstrap.profile"]),
            "os_authority_method": "bootstrap.os_administrator_service_handoff",
            "os_authority_requires": "root_or_administrator_only",
            "service_identity": service_identity,
            "service_group": service_group,
            "ownership_handoff": (
                "drop_to_service_identity_before_create"
                if os.name != "nt"
                else "explicit_managed_service_sid_acl"
            ),
        },
        "tls": {
            "required": True,
            "source": tls_source,
            "certificate": str(cert),
            "private_key_present": True,
            "private_key_embedded_in_manifest": False,
        },
        "dbbt": {
            "source": (
                "protected_windows_service_environment_bridge"
                if args.windows_system_service
                else "local_qa_keyring_environment_bridge"
            ),
            "service_delivery": dbbt_service_source,
            "key_file": str(dbbt_key),
            "key_present": True,
            "key_embedded_in_manifest": False,
        },
        "validation": validations if validations else "skipped_by_operator",
    }
    if args.windows_system_service:
        ownership_handoff = "installer_protected_managed_service_acl"
    else:
        ownership_handoff = handoff_windows_instance_acl(
            instance_root, service_identity
        )
    manifest_path = (
        instance_root / "install" / "NATIVE_QA_INSTANCE.json"
        if args.windows_system_service
        else instance_root / "NATIVE_QA_INSTANCE.json"
    )
    manifest["bootstrap"]["ownership_handoff"] = ownership_handoff
    manifest_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(f"prepare_native_qa_instance=passed:{instance_root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
