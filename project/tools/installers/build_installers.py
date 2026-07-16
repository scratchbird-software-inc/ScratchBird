#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Build ScratchBird installer artifacts from a staged public output tree."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import gzip
import hashlib
import json
import os
import plistlib
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
from typing import Any
from xml.sax.saxutils import escape
import zipfile


MANIFEST_NAME = "INSTALLER_ARTIFACT_MANIFEST.json"
PRODUCT_NAME = "ScratchBird"
MANUFACTURER = "ScratchBird Software Inc."
WINDOWS_UPGRADE_CODE = "8F28B062-0620-4D2A-8D4C-8D3E19ED4012"
WINDOWS_ASSET_ROOT = Path(__file__).resolve().parent / "windows"
WINDOWS_SYSTEM_ASSETS = {
    "scratchbird-windows-system-install.ps1": (
        "libexec/scratchbird-windows-system-install.ps1"
    ),
}
WINDOWS_SYSTEM_PROFILE_ASSET = "WINDOWS_SYSTEM_INSTALL_PROFILE.json"
WINDOWS_WIX_LIFECYCLE_TEMPLATE = "scratchbird-windows-lifecycle.wxs.in"
WINDOWS_CONFIG_DEFAULTS_REL = "share/scratchbird/config-defaults"
WINDOWS_CONFIG_ROOT = r"%ProgramData%\ScratchBird\config"
WINDOWS_NATIVE_CONFIGS = (
    "SBsrv.conf",
    "SBgate.conf",
    "SBmgr.conf",
    "SBParser.conf",
    "SBbootstrap.profile",
)
MACOS_SUPPORT_MATRIX = {
    "schema_id": "scratchbird.macos_support_matrix.v1",
    "minimum_macos_version": "14.0",
    "deployment_target": "14.0",
    "runner_labels": {
        "x86_64": "macos-15-intel",
        "arm64": "macos-15",
    },
    "architectures": ["x86_64", "arm64"],
    "universal_artifact_policy": "optional_after_individual_architecture_artifacts_verify",
    "rosetta_policy": "arm64_release_proof_must_be_native_not_translated",
    "external_runtime_prerequisites": {
        "homebrew": [
            {
                "formula": "llvm",
                "minimum_major": 22,
                "homebrew_prefix_relative_path": "lib/libLLVM.dylib",
                "reason": "mandatory dynamic LLVM native-compilation backend",
            }
        ],
        "policy": "qa_packages_do_not_bundle_homebrew_llvm",
    },
    "service_packaging": {
        "portable_tar": "foreground_only_no_launchd_definitions",
        "system_pkg": "launchd_definitions_installed_disabled_unloaded",
    },
    "filesystem_layout": {
        "runtime": "/opt/ScratchBird",
        "configuration": "/Library/Application Support/ScratchBird",
        "portable_configuration": "/etc/scratchbird",
        "system_configuration": "/Library/Application Support/ScratchBird",
        "launchd": "/Library/LaunchDaemons",
        "data": "/var/lib/scratchbird",
        "logs": "/var/log/scratchbird",
        "runtime_state": "/var/run/scratchbird",
    },
}
MACOS_LAUNCHD_SERVICES = (
    ("com.scratchbird.sbsrv", "SBsrv", "SBsrv.conf"),
    ("com.scratchbird.sbmgr", "SBmgr", "SBmgr.conf"),
)
MACOS_ASSET_ROOT = Path(__file__).resolve().parent / "macos"
MACOS_SYSTEM_ASSETS = {
    "scratchbird-macos-system-install.sh": (
        "opt/ScratchBird/libexec/scratchbird-macos-system-install"
    ),
    "com.scratchbird.sbsrv.plist": (
        "Library/LaunchDaemons/com.scratchbird.sbsrv.plist"
    ),
    "com.scratchbird.sbmgr.plist": (
        "Library/LaunchDaemons/com.scratchbird.sbmgr.plist"
    ),
}
MACOS_SYSTEM_PROFILE_ASSET = "MACOS_SYSTEM_INSTALL_PROFILE.json"
MACOS_CONFIG_ROOT = "/Library/Application Support/ScratchBird"
MACOS_CONFIG_DEFAULTS_REL = "opt/ScratchBird/share/scratchbird/config-defaults"
MACOS_PKG_SCRIPTS = ("postinstall.in",)
MACOS_NATIVE_CONFIGS = (
    "SBsrv.conf",
    "SBgate.conf",
    "SBmgr.conf",
    "SBParser.conf",
    "SBbootstrap.profile",
)
LINUX_ASSET_ROOT = Path(__file__).resolve().parent / "linux"
LINUX_SYSTEM_ASSETS = {
    "scratchbird-sbsrv.service": "usr/lib/systemd/system/scratchbird-sbsrv.service",
    "scratchbird.sysusers.conf": "usr/lib/sysusers.d/scratchbird.conf",
    "scratchbird.tmpfiles.conf": "usr/lib/tmpfiles.d/scratchbird.conf",
    "scratchbird-system-install.sh": "usr/lib/scratchbird/scratchbird-system-install",
}
LINUX_SERVICE_NAME = "scratchbird-sbsrv.service"
LINUX_CONFIG_RESTORE_COMMANDS = r"""preserve_root=/var/lib/scratchbird/install/config-preserve
if [ -d "$preserve_root" ]; then
  if [ -L /etc/scratchbird ]; then
    echo "BOOTSTRAP.DIRECTORY_PERMISSION_INVALID: /etc/scratchbird must not be a symlink" >&2
    exit 1
  fi
  install -d -m 0750 -o root -g scratchbird /etc/scratchbird
  cp -an "$preserve_root"/. /etc/scratchbird/
  find /etc/scratchbird -xdev -type d -exec chown root:scratchbird {} +
  find /etc/scratchbird -xdev -type d -exec chmod 0750 {} +
  find /etc/scratchbird -xdev -type f -exec chown root:scratchbird {} +
  find /etc/scratchbird -xdev -type f -exec chmod 0640 {} +
  rm -rf -- "$preserve_root"
  evidence=/var/lib/scratchbird/install/LINUX_SYSTEM_UNINSTALL_STATE.json
  evidence_tmp=$evidence.$$
  printf '%s\n' '{"schema_id":"scratchbird.linux_system_uninstall_state.v1","configuration_preserved":true,"data_preserved":true}' > "$evidence_tmp"
  chmod 0640 "$evidence_tmp"
  chown root:scratchbird "$evidence_tmp"
  mv -f "$evidence_tmp" "$evidence"
fi"""
FORBIDDEN_TEXT = (
    "ScratchBird" + "-Private",
    "/home/",
    "\\home\\",
    "/local" + "_work",
    "\\local" + "_work",
    "docs/workplans",
    "docs/specifications",
    "project/tests/reference_regression/reference_release_acquisition/",
    "packaging/",
)

DEBIAN_RUNTIME_DEPENDENCIES = (
    "libc6",
    "libstdc++6",
    "libgcc-s1",
    "libssl3t64 | libssl3",
    "libicu74",
    "libquadmath0",
    "libllvm23",
    "passwd",
    "systemd",
)

RPM_RUNTIME_REQUIREMENTS = (
    "glibc",
    "libstdc++",
    "openssl-libs",
    "libicu",
    "libquadmath",
    "llvm-libs >= 23",
    "shadow-utils",
    "systemd",
)

AUR_RUNTIME_DEPENDENCIES = (
    "glibc",
    "gcc-libs",
    "openssl",
    "icu",
    "llvm-libs>=23",
    "shadow",
    "systemd",
)


def fail(message: str) -> None:
    print(f"build_installers=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def run(command: list[str], *, cwd: Path) -> str:
    result = subprocess.run(
        command,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode != 0:
        print(result.stdout, end="")
        fail(f"command_failed:{command[0]}:exit={result.returncode}")
    return result.stdout


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sanitize_version(version: str) -> str:
    value = re.sub(r"[^0-9A-Za-z.+~_-]", ".", version.strip())
    value = value.strip(".-_")
    return value or "0.0.0-nightly"


def rpm_version(version: str) -> tuple[str, str]:
    value = sanitize_version(version).replace("-", "_")
    match = re.match(r"^([0-9]+(?:[.][0-9]+)*)(.*)$", value)
    if not match:
        return "0.0.0", "1"
    base = match.group(1)
    suffix = match.group(2).strip("._+~")
    release = "1" if not suffix else f"1.{re.sub(r'[^0-9A-Za-z_]', '_', suffix)}"
    return base, release


def windows_msi_version(version: str) -> str:
    """Convert a pre-release version into the numeric three-part MSI form."""
    parts = [int(part) for part in re.findall(r"[0-9]+", sanitize_version(version))]
    parts = (parts + [0, 0, 0])[:3]
    return ".".join(str(min(part, 65535)) for part in parts)


def is_text_candidate(path: Path) -> bool:
    if path.stat().st_size > 2 * 1024 * 1024:
        return False
    if path.suffix.lower() in {".json", ".md", ".txt", ".csv", ".xml", ".ini", ".conf", ".service", ".plist", ".sh", ".ps1"}:
        return True
    return path.name in {
        "SHA256SUMS",
        "PKGBUILD",
        "control",
        "conffiles",
        "postinst",
        "postrm",
        "prerm",
        "scratchbird-system-install",
        "scratchbird.install",
    }


def scan_private_text(root: Path) -> None:
    for path in sorted(item for item in root.rglob("*") if item.is_file()):
        rel = path.relative_to(root).as_posix()
        for forbidden in FORBIDDEN_TEXT:
            if forbidden in rel:
                fail(f"forbidden_path_fragment:{rel}:{forbidden}")
        if not is_text_candidate(path):
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        for forbidden in FORBIDDEN_TEXT:
            if forbidden in text:
                fail(f"forbidden_text_fragment:{rel}:{forbidden}")


def require_staged_output(
    artifact_root: Path,
    platform: str,
    require_native_only: bool = False,
) -> None:
    if not artifact_root.is_dir():
        fail(f"artifact_root_not_found:{artifact_root}")
    manifest = artifact_root / "STANDALONE_OUTPUT_MANIFEST.json"
    if not manifest.is_file():
        fail(f"missing_standalone_manifest:{manifest}")
    try:
        data = json.loads(manifest.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        fail(f"standalone_manifest_invalid:{exc}")
    if data.get("platform") != platform:
        fail(f"standalone_manifest_platform_mismatch:{data.get('platform')}:{platform}")
    if require_native_only:
        if data.get("distribution_profile") != "native-sbsql-only":
            fail("native_only_distribution_profile_missing")
        if data.get("emulation_components") != "excluded":
            fail("native_only_emulation_exclusion_missing")
        native_profile = artifact_root / "NATIVE_RELEASE_PROFILE.json"
        if not native_profile.is_file():
            fail(f"native_only_profile_missing:{native_profile}")
    for rel in ("bin", "lib", "etc/scratchbird", "share/scratchbird/resources"):
        if not (artifact_root / rel).exists():
            fail(f"staged_output_missing:{rel}")


def copytree_contents(source: Path, dest: Path) -> None:
    if not source.exists():
        return
    dest.mkdir(parents=True, exist_ok=True)
    for child in sorted(source.iterdir()):
        target = dest / child.name
        if child.is_dir():
            shutil.copytree(child, target, dirs_exist_ok=True)
        elif child.is_file():
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(child, target)


def sanitize_release_manifest(source: Path, target: Path, platform: str) -> None:
    """Copy a generated release manifest without leaking local build paths."""
    try:
        data = json.loads(source.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        shutil.copy2(source, target)
        return
    if isinstance(data, dict):
        for key in ("artifact_root", "build_root", "source_root", "output_root"):
            if key in data:
                data[key] = f"<scratchbird-{platform}-release-artifact-root>"
        source_block = data.get("source")
        if isinstance(source_block, dict):
            for key in ("root", "path", "worktree", "repository"):
                if key in source_block and isinstance(source_block[key], str):
                    source_block[key] = "<scratchbird-public-source-checkout>"
    target.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def collect_install_files(root: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for path in sorted(item for item in root.rglob("*") if item.is_file()):
        rel = path.relative_to(root).as_posix()
        if rel.endswith("INSTALL_MANIFEST.json") or rel.endswith("SHA256SUMS"):
            continue
        rows.append({"path": rel, "bytes": path.stat().st_size, "sha256": sha256_file(path)})
    return rows


def write_install_metadata(
    root: Path,
    platform: str,
    version: str,
    build_id: str | None,
    *,
    configuration_root: str = "/etc/scratchbird",
) -> None:
    release_dir = root / "opt" / "ScratchBird" / "share" / "scratchbird" / "release"
    release_dir.mkdir(parents=True, exist_ok=True)
    manifest = {
        "schema_id": "scratchbird.installer_payload_manifest.v1",
        "generated_at_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
        "product": PRODUCT_NAME,
        "platform": platform,
        "version": version,
        "build_id": build_id,
        "install_roots": {
            "runtime": "/opt/ScratchBird",
            "configuration": configuration_root,
        },
        "files": collect_install_files(root),
    }
    (release_dir / "INSTALL_MANIFEST.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    rows = collect_install_files(root)
    sha_lines = [f"{row['sha256']}  {row['path']}" for row in rows]
    (release_dir / "SHA256SUMS").write_text("\n".join(sha_lines) + "\n", encoding="utf-8")


def write_macos_support_matrix(root: Path) -> None:
    release_dir = root / "opt" / "ScratchBird" / "share" / "scratchbird" / "release"
    release_dir.mkdir(parents=True, exist_ok=True)
    (release_dir / "MACOS_SUPPORT_MATRIX.json").write_text(
        json.dumps(MACOS_SUPPORT_MATRIX, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def replace_required_text(
    path: Path,
    replacements: dict[str, str],
    *,
    contract: str = "linux_system",
) -> None:
    """Apply exact, fail-closed system-package path substitutions."""
    text = path.read_text(encoding="utf-8")
    for source, target in replacements.items():
        count = text.count(source)
        if count != 1:
            fail(
                f"{contract}_config_assignment_mismatch:"
                f"{path.name}:{source}:count={count}"
            )
        text = text.replace(source, target, 1)
    path.write_text(text, encoding="utf-8")


def write_linux_system_install_profile(
    root: Path,
    version: str,
    build_id: str | None,
) -> Path:
    release_dir = root / "opt" / "ScratchBird" / "share" / "scratchbird" / "release"
    release_dir.mkdir(parents=True, exist_ok=True)
    profile = {
        "schema_id": "scratchbird.linux_system_install_profile.v1",
        "product": PRODUCT_NAME,
        "version": version,
        "build_id": build_id,
        "distribution_profile": "native-sbsql-only",
        "native_default_port": 3092,
        "service": {
            "unit": LINUX_SERVICE_NAME,
            "binary": "/opt/ScratchBird/bin/SBsrv",
            "owns_listener_and_parser_children": True,
            "default_enablement": "disabled",
            "default_activity": "not_started",
            "create_if_missing": False,
        },
        "os_identity": {
            "user": "scratchbird",
            "group": "scratchbird",
            "login": "forbidden",
            "home": "/var/lib/scratchbird",
            "human_service_group_membership_mutation": "forbidden",
            "create_time_os_authorization": "root_only",
            "service_authority_scope": (
                "filesystem_directory_and_process_execution_only_"
                "no_database_or_security_authority"
            ),
            "package_manager_never_adds_human_group_members": True,
            "existing_identity_validation": [
                "unique_local_passwd_group_shadow_records",
                "nonzero_unique_system_uid_gid",
                "scratchbird_primary_group",
                "only_scratchbird_effective_group",
                "exact_service_home",
                "approved_non_login_shell",
                "locked_shadow_password",
            ],
        },
        "directories": [
            {"path": "/etc/scratchbird", "owner": "root", "group": "scratchbird", "mode": "0750"},
            {"path": "/var/lib/scratchbird", "owner": "root", "group": "scratchbird", "mode": "0750"},
            {"path": "/var/lib/scratchbird/data", "owner": "scratchbird", "group": "scratchbird", "mode": "0750"},
            {"path": "/var/lib/scratchbird/install", "owner": "root", "group": "scratchbird", "mode": "0750"},
            {"path": "/var/log/scratchbird", "owner": "scratchbird", "group": "scratchbird", "mode": "0750"},
            {"path": "/run/scratchbird", "owner": "scratchbird", "group": "scratchbird", "mode": "0750"},
            {"path": "/run/scratchbird/control", "owner": "scratchbird", "group": "scratchbird", "mode": "0750"},
            {"path": "/run/scratchbird/runtime", "owner": "scratchbird", "group": "scratchbird", "mode": "0750"},
            {"path": "/run/scratchbird/listener", "owner": "scratchbird", "group": "scratchbird", "mode": "0750"},
            {"path": "/run/scratchbird/listener/control", "owner": "scratchbird", "group": "scratchbird", "mode": "0750"},
            {"path": "/run/scratchbird/listener/runtime", "owner": "scratchbird", "group": "scratchbird", "mode": "0750"},
            {"path": "/run/scratchbird/manager", "owner": "scratchbird", "group": "scratchbird", "mode": "0750"},
            {"path": "/run/scratchbird/manager/control", "owner": "scratchbird", "group": "scratchbird", "mode": "0750"},
            {"path": "/run/scratchbird/manager/runtime", "owner": "scratchbird", "group": "scratchbird", "mode": "0750"},
        ],
        "lifecycle": {
            "identity_and_directory_setup": "/usr/lib/scratchbird/scratchbird-system-install",
            "sysusers": "/usr/lib/sysusers.d/scratchbird.conf",
            "tmpfiles": "/usr/lib/tmpfiles.d/scratchbird.conf",
            "install_evidence": "/var/lib/scratchbird/install/LINUX_SYSTEM_INSTALL_STATE.json",
            "human_service_group_membership_mutation": False,
            "database_files_created": False,
            "security_sidecars_created": False,
            "upgrade_preserves_operator_service_state": True,
            "uninstall_preserves_config_data_logs_and_identity": True,
        },
    }
    path = release_dir / "LINUX_SYSTEM_INSTALL_PROFILE.json"
    path.write_text(json.dumps(profile, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    path.chmod(0o644)
    return path


def stage_linux_system_install_tree(
    portable_root: Path,
    system_root: Path,
    version: str,
    build_id: str | None,
) -> None:
    """Derive a system-package tree without changing the portable tar payload."""
    if system_root.exists():
        shutil.rmtree(system_root)
    shutil.copytree(portable_root, system_root, symlinks=True)

    for source_name, target_rel in LINUX_SYSTEM_ASSETS.items():
        source = LINUX_ASSET_ROOT / source_name
        if not source.is_file():
            fail(f"linux_system_asset_missing:{source}")
        target = system_root / target_rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        target.chmod(0o755 if source_name.endswith(".sh") else 0o644)

    config_root = system_root / "etc" / "scratchbird"
    replace_required_text(
        config_root / "SBsrv.conf",
        {
            "data_dir = runtime/data": "data_dir = /run/scratchbird/runtime",
            "control_dir = runtime/control": "control_dir = /run/scratchbird/control",
            "log_file = stderr": "log_file = /var/log/scratchbird/SBsrv.log",
            "default_path = data/default.sbdb": "default_path = /var/lib/scratchbird/data/default.sbdb",
            "executable_path = bin/SBgate": "executable_path = /opt/ScratchBird/bin/SBgate",
            "parser_executable_path = bin/SBParser": "parser_executable_path = /opt/ScratchBird/bin/SBParser",
            "control_dir = runtime/listener/control": "control_dir = /run/scratchbird/listener/control",
            "runtime_dir = runtime/listener/runtime": "runtime_dir = /run/scratchbird/listener/runtime",
            "sbps_endpoint = runtime/control/sb_server.sbps.sock": "sbps_endpoint = /run/scratchbird/control/sb_server.sbps.sock",
        },
    )
    replace_required_text(
        config_root / "SBgate.conf",
        {
            "parser_executable = bin/SBParser": "parser_executable = /opt/ScratchBird/bin/SBParser",
            "server_endpoint = runtime/control/sb_server.sbps.sock": "server_endpoint = /run/scratchbird/control/sb_server.sbps.sock",
            "control_dir = runtime/listener/control": "control_dir = /run/scratchbird/listener/control",
            "runtime_dir = runtime/listener/runtime": "runtime_dir = /run/scratchbird/listener/runtime",
        },
    )
    replace_required_text(
        config_root / "SBmgr.conf",
        {
            "manager.runtime_dir = runtime/manager/runtime": "manager.runtime_dir = /run/scratchbird/manager/runtime",
            "manager.control_dir = runtime/manager/control": "manager.control_dir = /run/scratchbird/manager/control",
            "manager.log.path = stderr": "manager.log.path = /var/log/scratchbird/SBmgr.log",
            "manager.owner.database_path = data/default.sbdb": "manager.owner.database_path = /var/lib/scratchbird/data/default.sbdb",
        },
    )
    replace_required_text(
        config_root / "SBParser.conf",
        {
            "parser.worker_binary = bin/SBParser": (
                "parser.worker_binary = /opt/ScratchBird/bin/SBParser"
            ),
        },
    )
    replace_required_text(
        config_root / "SBbootstrap.profile",
        {
            "platform = operator_required": "platform = linux",
            "service_identity = operator_required": (
                "service_identity = scratchbird"
            ),
            "service_group = operator_required": "service_group = scratchbird",
        },
    )
    for path in sorted(item for item in config_root.rglob("*") if item.is_file()):
        path.chmod(0o640)
    for path in sorted(item for item in config_root.rglob("*") if item.is_dir()):
        path.chmod(0o750)
    config_root.chmod(0o750)

    write_linux_system_install_profile(system_root, version, build_id)
    write_install_metadata(system_root, "linux", version, build_id)
    release_dir = system_root / "opt" / "ScratchBird" / "share" / "scratchbird" / "release"
    for path in sorted(item for item in release_dir.rglob("*") if item.is_file()):
        path.chmod(0o644)
    scan_private_text(system_root)


def write_windows_system_install_profile(
    root: Path,
    version: str,
    build_id: str | None,
) -> Path:
    source = WINDOWS_ASSET_ROOT / WINDOWS_SYSTEM_PROFILE_ASSET
    if not source.is_file():
        fail(f"windows_system_profile_asset_missing:{source}")
    try:
        profile = json.loads(source.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        fail(f"windows_system_profile_asset_invalid:{exc}")
    if profile.get("schema_id") != "scratchbird.windows_system_install_profile.v1":
        fail("windows_system_profile_asset_schema_mismatch")
    if profile.get("native_default_port") != 3092:
        fail("windows_system_profile_native_port_mismatch")
    profile["product"] = PRODUCT_NAME
    profile["version"] = version
    profile["build_id"] = build_id
    release_dir = root / "share" / "scratchbird" / "release"
    release_dir.mkdir(parents=True, exist_ok=True)
    target = release_dir / WINDOWS_SYSTEM_PROFILE_ASSET
    target.write_text(
        json.dumps(profile, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return target


def write_windows_install_metadata(
    root: Path,
    version: str,
    build_id: str | None,
) -> Path:
    release_dir = root / "share" / "scratchbird" / "release"
    release_dir.mkdir(parents=True, exist_ok=True)
    manifest = {
        "schema_id": "scratchbird.installer_payload_manifest.v1",
        "generated_at_utc": datetime.now(timezone.utc).replace(
            microsecond=0
        ).isoformat(),
        "product": PRODUCT_NAME,
        "platform": "windows",
        "version": version,
        "build_id": build_id,
        "install_roots": {
            "runtime": r"%ProgramFiles%\ScratchBird",
            "configuration": WINDOWS_CONFIG_ROOT,
            "state": r"%ProgramData%\ScratchBird",
        },
        "files": collect_install_files(root),
    }
    path = release_dir / "INSTALL_MANIFEST.json"
    path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    rows = collect_install_files(root)
    (release_dir / "SHA256SUMS").write_text(
        "\n".join(f"{row['sha256']}  {row['path']}" for row in rows) + "\n",
        encoding="utf-8",
    )
    return path


def stage_windows_system_install_tree(
    portable_root: Path,
    system_root: Path,
    version: str,
    build_id: str | None,
) -> None:
    """Derive the MSI system tree without changing the portable Windows ZIP."""
    if system_root.exists():
        shutil.rmtree(system_root)
    runtime_root = portable_root / "opt" / "ScratchBird"
    if not runtime_root.is_dir():
        fail("windows_portable_runtime_root_missing")
    shutil.copytree(runtime_root, system_root, symlinks=False)

    portable_config_root = portable_root / "etc" / "scratchbird"
    if not portable_config_root.is_dir():
        fail("windows_portable_config_root_missing")
    actual_configs: set[str] = set()
    for path in sorted(portable_config_root.iterdir()):
        if path.is_symlink() or not path.is_file():
            fail(f"windows_portable_config_entry_invalid:{path.name}")
        actual_configs.add(path.name)
    if actual_configs != set(WINDOWS_NATIVE_CONFIGS):
        fail(
            "windows_portable_config_set_mismatch:"
            f"missing={sorted(set(WINDOWS_NATIVE_CONFIGS) - actual_configs)}:"
            f"unexpected={sorted(actual_configs - set(WINDOWS_NATIVE_CONFIGS))}"
        )
    defaults_root = system_root / WINDOWS_CONFIG_DEFAULTS_REL
    if defaults_root.exists():
        shutil.rmtree(defaults_root)
    shutil.copytree(portable_config_root, defaults_root, symlinks=False)

    state = "@SCRATCHBIRD_STATE_ROOT@"
    install = "@SCRATCHBIRD_INSTALL_ROOT@"
    replace_required_text(
        defaults_root / "SBsrv.conf",
        {
            "data_dir = runtime/data": f"data_dir = {state}/run/sb_server",
            "control_dir = runtime/control": (
                f"control_dir = {state}/run/sb_server/control"
            ),
            "log_file = stderr": f"log_file = {state}/log/SBsrv.log",
            "default_path = data/default.sbdb": (
                f"default_path = {state}/data/default.sbdb"
            ),
            "executable_path = bin/SBgate": (
                f"executable_path = {install}/bin/SBgate.exe"
            ),
            "parser_executable_path = bin/SBParser": (
                f"parser_executable_path = {install}/bin/SBParser.exe"
            ),
            "control_dir = runtime/listener/control": (
                f"control_dir = {state}/run/listener/control"
            ),
            "runtime_dir = runtime/listener/runtime": (
                f"runtime_dir = {state}/run/listener/runtime"
            ),
            "sbps_endpoint = runtime/control/sb_server.sbps.sock": (
                f"sbps_endpoint = {state}/run/sb_server/control/"
                "sb_server.sbps.sock"
            ),
        },
        contract="windows_system",
    )
    replace_required_text(
        defaults_root / "SBgate.conf",
        {
            "parser_executable = bin/SBParser": (
                f"parser_executable = {install}/bin/SBParser.exe"
            ),
            "server_endpoint = runtime/control/sb_server.sbps.sock": (
                f"server_endpoint = {state}/run/sb_server/control/"
                "sb_server.sbps.sock"
            ),
            "control_dir = runtime/listener/control": (
                f"control_dir = {state}/run/listener/control"
            ),
            "runtime_dir = runtime/listener/runtime": (
                f"runtime_dir = {state}/run/listener/runtime"
            ),
        },
        contract="windows_system",
    )
    replace_required_text(
        defaults_root / "SBmgr.conf",
        {
            "manager.runtime_dir = runtime/manager/runtime": (
                f"manager.runtime_dir = {state}/run/manager/runtime"
            ),
            "manager.control_dir = runtime/manager/control": (
                f"manager.control_dir = {state}/run/manager/control"
            ),
            "manager.log.path = stderr": (
                f"manager.log.path = {state}/log/SBmgr.log"
            ),
            "manager.owner.database_path = data/default.sbdb": (
                f"manager.owner.database_path = {state}/data/default.sbdb"
            ),
        },
        contract="windows_system",
    )
    replace_required_text(
        defaults_root / "SBParser.conf",
        {
            "parser.worker_binary = bin/SBParser": (
                f"parser.worker_binary = {install}/bin/SBParser.exe"
            ),
        },
        contract="windows_system",
    )
    replace_required_text(
        defaults_root / "SBbootstrap.profile",
        {
            "platform = operator_required": "platform = windows",
            "service_identity = operator_required": (
                r"service_identity = NT SERVICE\scratchbird"
            ),
            "service_group = operator_required": (
                "service_group = ScratchBird"
            ),
        },
        contract="windows_system",
    )

    for config in WINDOWS_NATIVE_CONFIGS:
        text = (defaults_root / config).read_text(encoding="utf-8")
        if "3050" in text:
            fail(f"windows_native_firebird_port_leak:{config}")
    for source_name, target_rel in WINDOWS_SYSTEM_ASSETS.items():
        source = WINDOWS_ASSET_ROOT / source_name
        if not source.is_file():
            fail(f"windows_system_asset_missing:{source}")
        target = system_root / target_rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)

    write_windows_system_install_profile(system_root, version, build_id)
    write_windows_install_metadata(system_root, version, build_id)
    scan_private_text(system_root)


def write_macos_system_install_profile(
    root: Path,
    version: str,
    build_id: str | None,
) -> Path:
    source = MACOS_ASSET_ROOT / MACOS_SYSTEM_PROFILE_ASSET
    if not source.is_file():
        fail(f"macos_system_profile_asset_missing:{source}")
    try:
        profile = json.loads(source.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        fail(f"macos_system_profile_asset_invalid:{exc}")
    if profile.get("schema_id") != "scratchbird.macos_system_install_profile.v1":
        fail("macos_system_profile_asset_schema_mismatch")
    profile["product"] = PRODUCT_NAME
    profile["version"] = version
    profile["build_id"] = build_id
    release_dir = root / "opt" / "ScratchBird" / "share" / "scratchbird" / "release"
    release_dir.mkdir(parents=True, exist_ok=True)
    target = release_dir / MACOS_SYSTEM_PROFILE_ASSET
    target.write_text(
        json.dumps(profile, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    target.chmod(0o644)
    return target


def write_macos_system_launchd_manifest(root: Path) -> Path:
    launchd_root = root / "Library" / "LaunchDaemons"
    rows: list[dict[str, Any]] = []
    for label, binary, config in MACOS_LAUNCHD_SERVICES:
        plist_path = launchd_root / f"{label}.plist"
        if not plist_path.is_file():
            fail(f"macos_system_launchd_asset_missing:{plist_path}")
        try:
            payload = plistlib.loads(plist_path.read_bytes())
        except plistlib.InvalidFileException as exc:
            fail(f"macos_system_launchd_asset_invalid:{plist_path.name}:{exc}")
        if payload.get("Label") != label:
            fail(f"macos_system_launchd_label_mismatch:{plist_path.name}")
        arguments = payload.get("ProgramArguments")
        expected_config = f"{MACOS_CONFIG_ROOT}/{config}"
        if (
            not isinstance(arguments, list)
            or "--config" not in arguments
            or expected_config not in arguments
        ):
            fail(f"macos_system_launchd_config_mismatch:{plist_path.name}")
        if (
            payload.get("UserName") != "scratchbird"
            or payload.get("GroupName") != "scratchbird"
            or payload.get("RunAtLoad") is not False
            or payload.get("KeepAlive") is not False
            or payload.get("Disabled") is not True
        ):
            fail(f"macos_system_launchd_policy_mismatch:{plist_path.name}")
        rows.append(
            {
                "label": label,
                "binary": binary,
                "config": expected_config,
                "plist": f"/Library/LaunchDaemons/{plist_path.name}",
                "user": "scratchbird",
                "group": "scratchbird",
                "run_at_load": False,
                "disabled": True,
            }
        )
    release_dir = root / "opt" / "ScratchBird" / "share" / "scratchbird" / "release"
    release_dir.mkdir(parents=True, exist_ok=True)
    target = release_dir / "MACOS_LAUNCHD_MANIFEST.json"
    target.write_text(
        json.dumps(
            {
                "schema_id": "scratchbird.macos_launchd_manifest.v1",
                "services": rows,
                "default_service_state": "installed_disabled_unloaded_not_run_at_load",
                "child_process_ownership": (
                    "SBsrv_to_shared_SBgate_to_standalone_SBParser"
                ),
            },
            indent=2,
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )
    target.chmod(0o644)
    return target


def stage_macos_system_install_tree(
    portable_root: Path,
    system_root: Path,
    version: str,
    build_id: str | None,
) -> None:
    """Derive the canonical macOS pkg tree without changing the portable tar."""
    if system_root.exists():
        shutil.rmtree(system_root)
    shutil.copytree(portable_root, system_root, symlinks=True)

    portable_config_root = system_root / "etc" / "scratchbird"
    if not portable_config_root.is_dir():
        fail("macos_portable_config_root_missing")
    actual_configs: set[str] = set()
    for path in sorted(portable_config_root.iterdir()):
        if path.is_symlink() or not path.is_file():
            fail(f"macos_portable_config_entry_invalid:{path.name}")
        actual_configs.add(path.name)
    if actual_configs != set(MACOS_NATIVE_CONFIGS):
        fail(
            "macos_portable_config_set_mismatch:"
            f"missing={sorted(set(MACOS_NATIVE_CONFIGS) - actual_configs)}:"
            f"unexpected={sorted(actual_configs - set(MACOS_NATIVE_CONFIGS))}"
        )
    defaults_root = system_root / MACOS_CONFIG_DEFAULTS_REL
    if defaults_root.exists():
        shutil.rmtree(defaults_root)
    shutil.copytree(portable_config_root, defaults_root, symlinks=False)

    replace_required_text(
        defaults_root / "SBsrv.conf",
        {
            "data_dir = runtime/data": (
                "data_dir = /var/run/scratchbird/sb_server"
            ),
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
        contract="macos_system",
    )
    replace_required_text(
        defaults_root / "SBgate.conf",
        {
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
        contract="macos_system",
    )
    replace_required_text(
        defaults_root / "SBmgr.conf",
        {
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
                "manager.owner.database_path = /var/lib/scratchbird/data/"
                "default.sbdb"
            ),
        },
        contract="macos_system",
    )
    replace_required_text(
        defaults_root / "SBParser.conf",
        {
            "parser.worker_binary = bin/SBParser": (
                "parser.worker_binary = /opt/ScratchBird/bin/SBParser"
            ),
        },
        contract="macos_system",
    )
    replace_required_text(
        defaults_root / "SBbootstrap.profile",
        {
            "platform = operator_required": "platform = macos",
            "service_identity = operator_required": (
                "service_identity = scratchbird"
            ),
            "service_group = operator_required": "service_group = scratchbird",
        },
        contract="macos_system",
    )
    for path in sorted(item for item in defaults_root.rglob("*") if item.is_file()):
        path.chmod(0o644)
    for path in sorted(item for item in defaults_root.rglob("*") if item.is_dir()):
        path.chmod(0o755)
    defaults_root.chmod(0o755)

    shutil.rmtree(portable_config_root)
    portable_etc_root = system_root / "etc"
    if portable_etc_root.is_dir() and not any(portable_etc_root.iterdir()):
        portable_etc_root.rmdir()
    portable_var_root = system_root / "opt" / "ScratchBird" / "var"
    if portable_var_root.exists():
        shutil.rmtree(portable_var_root)

    for source_name, target_rel in MACOS_SYSTEM_ASSETS.items():
        source = MACOS_ASSET_ROOT / source_name
        if not source.is_file():
            fail(f"macos_system_asset_missing:{source}")
        target = system_root / target_rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        target.chmod(0o755 if source_name.endswith(".sh") else 0o644)

    write_macos_system_install_profile(system_root, version, build_id)
    write_macos_system_launchd_manifest(system_root)
    write_install_metadata(
        system_root,
        "macos",
        version,
        build_id,
        configuration_root=MACOS_CONFIG_ROOT,
    )
    release_dir = system_root / "opt" / "ScratchBird" / "share" / "scratchbird" / "release"
    for path in sorted(item for item in release_dir.rglob("*") if item.is_file()):
        path.chmod(0o644)
    scan_private_text(system_root)


def materialize_macos_pkg_scripts(scripts_root: Path, version: str) -> Path:
    if scripts_root.exists():
        shutil.rmtree(scripts_root)
    scripts_root.mkdir(parents=True)
    for template_name in MACOS_PKG_SCRIPTS:
        source = MACOS_ASSET_ROOT / "pkg-scripts" / template_name
        if not source.is_file():
            fail(f"macos_pkg_script_asset_missing:{source}")
        text = source.read_text(encoding="utf-8")
        if text.count("@SCRATCHBIRD_VERSION@") != 1:
            fail(f"macos_pkg_script_version_token_mismatch:{template_name}")
        target = scripts_root / template_name.removesuffix(".in")
        target.write_text(
            text.replace("@SCRATCHBIRD_VERSION@", sanitize_version(version), 1),
            encoding="utf-8",
        )
        target.chmod(0o755)
    scan_private_text(scripts_root)
    return scripts_root


def stage_install_tree(artifact_root: Path, payload_root: Path, platform: str, version: str, build_id: str | None) -> None:
    if payload_root.exists():
        shutil.rmtree(payload_root)
    (payload_root / "opt" / "ScratchBird").mkdir(parents=True)
    copytree_contents(artifact_root / "bin", payload_root / "opt" / "ScratchBird" / "bin")
    copytree_contents(artifact_root / "lib", payload_root / "opt" / "ScratchBird" / "lib")
    copytree_contents(artifact_root / "share", payload_root / "opt" / "ScratchBird" / "share")
    copytree_contents(artifact_root / "etc", payload_root / "etc")
    for file_name in (
        "STANDALONE_OUTPUT_MANIFEST.json",
        "PUBLIC_RELEASE_ARTIFACT_MANIFEST.json",
        "NATIVE_RELEASE_PROFILE.json",
    ):
        source = artifact_root / file_name
        if source.is_file():
            target = payload_root / "opt" / "ScratchBird" / "share" / "scratchbird" / "release" / file_name
            target.parent.mkdir(parents=True, exist_ok=True)
            sanitize_release_manifest(source, target, platform)
    if platform == "macos":
        write_macos_support_matrix(payload_root)
    write_install_metadata(payload_root, platform, version, build_id)
    scan_private_text(payload_root)


def make_tarball(payload_root: Path, output_root: Path, version: str, platform: str) -> Path:
    output = output_root / f"scratchbird-{platform}-{version}.tar.gz"
    with tarfile.open(output, "w:gz", format=tarfile.PAX_FORMAT) as archive:
        for path in sorted(payload_root.rglob("*")):
            archive.add(path, arcname=path.relative_to(payload_root).as_posix(), recursive=False)
    return output


def normalize_tar_owner(info: tarfile.TarInfo) -> tarfile.TarInfo:
    info.uid = 0
    info.gid = 0
    info.uname = "root"
    info.gname = "root"
    return info


def tar_bytes_from_dir(
    root: Path,
    mode: str = "w:gz",
    *,
    normalize_ownership: bool = False,
) -> bytes:
    temp = tempfile.NamedTemporaryFile(delete=False)
    temp.close()
    temp_path = Path(temp.name)
    try:
        with tarfile.open(temp_path, mode, format=tarfile.PAX_FORMAT) as archive:
            for path in sorted(root.rglob("*")):
                archive.add(
                    path,
                    arcname=path.relative_to(root).as_posix(),
                    recursive=False,
                    filter=normalize_tar_owner if normalize_ownership else None,
                )
        return temp_path.read_bytes()
    finally:
        temp_path.unlink(missing_ok=True)


def ar_member(name: str, payload: bytes, mode: int = 0o100644) -> bytes:
    encoded = name.encode("ascii")
    if len(encoded) > 15:
        fail(f"ar_name_too_long:{name}")
    header = (
        encoded.ljust(16, b" ")
        + b"0".ljust(12, b" ")
        + b"0".ljust(6, b" ")
        + b"0".ljust(6, b" ")
        + oct(mode)[2:].encode("ascii").ljust(8, b" ")
        + str(len(payload)).encode("ascii").ljust(10, b" ")
        + b"`\n"
    )
    if len(payload) % 2:
        payload += b"\n"
    return header + payload


def make_deb(payload_root: Path, output_root: Path, version: str) -> Path:
    control_root = output_root / ".deb-control"
    if control_root.exists():
        shutil.rmtree(control_root)
    control_root.mkdir(parents=True)
    installed_size = sum(path.stat().st_size for path in payload_root.rglob("*") if path.is_file()) // 1024
    (control_root / "control").write_text(
        "\n".join(
            [
                "Package: scratchbird",
                f"Version: {sanitize_version(version).replace('_', '-')}",
                "Section: database",
                "Priority: optional",
                "Architecture: amd64",
                "Maintainer: ScratchBird Software Inc. <support@scratchbird.com>",
                f"Depends: {', '.join(DEBIAN_RUNTIME_DEPENDENCIES)}",
                f"Installed-Size: {max(installed_size, 1)}",
                "Description: ScratchBird Convergent Data Engine pre-release build",
                "",
            ]
        ),
        encoding="utf-8",
    )
    deb_version = sanitize_version(version).replace("_", "-")
    lifecycle = "/usr/lib/scratchbird/scratchbird-system-install"
    scripts = {
        "postinst": f"""#!/bin/sh
set -eu
case "${{1:-}}" in
  configure|triggered)
    {lifecycle} post-install --package-format deb --package-version {deb_version}
    ;;
esac
exit 0
""",
        "prerm": f"""#!/bin/sh
set -eu
case "${{1:-}}" in
  remove|deconfigure)
    if [ -x {lifecycle} ]; then
      {lifecycle} pre-remove --package-format deb --package-version {deb_version}
    fi
    ;;
esac
exit 0
""",
        "postrm": f"""#!/bin/sh
set -eu
case "${{1:-}}" in
  remove|purge)
    if command -v systemctl >/dev/null 2>&1; then
      systemctl daemon-reload >/dev/null 2>&1 || true
    fi
    rmdir /run/scratchbird/listener/control 2>/dev/null || true
    rmdir /run/scratchbird/listener/runtime 2>/dev/null || true
    rmdir /run/scratchbird/listener 2>/dev/null || true
    rmdir /run/scratchbird/manager/control 2>/dev/null || true
    rmdir /run/scratchbird/manager/runtime 2>/dev/null || true
    rmdir /run/scratchbird/manager 2>/dev/null || true
    rmdir /run/scratchbird/control 2>/dev/null || true
    rmdir /run/scratchbird/runtime 2>/dev/null || true
    rmdir /run/scratchbird 2>/dev/null || true
    if [ "${{1:-}}" = remove ]; then
{LINUX_CONFIG_RESTORE_COMMANDS}
    fi
    ;;
esac
exit 0
""",
    }
    for script_name, text in scripts.items():
        path = control_root / script_name
        path.write_text(text, encoding="utf-8")
        path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    config_files = sorted(
        path.relative_to(payload_root).as_posix()
        for path in (payload_root / "etc" / "scratchbird").rglob("*")
        if path.is_file()
    )
    (control_root / "conffiles").write_text(
        "".join(f"/{path}\n" for path in config_files),
        encoding="utf-8",
    )

    deb = output_root / f"scratchbird_{sanitize_version(version).replace('-', '+')}_amd64.deb"
    control_tar = tar_bytes_from_dir(control_root, normalize_ownership=True)
    data_tar = tar_bytes_from_dir(payload_root, normalize_ownership=True)
    with deb.open("wb") as handle:
        handle.write(b"!<arch>\n")
        handle.write(ar_member("debian-binary", b"2.0\n"))
        handle.write(ar_member("control.tar.gz", control_tar))
        handle.write(ar_member("data.tar.gz", data_tar))
    shutil.rmtree(control_root)
    return deb


def make_rpm(payload_root: Path, output_root: Path, version: str, require_rpm: bool) -> list[Path]:
    rpm_bin = shutil.which("rpmbuild")
    topdir = output_root / "rpm-build"
    if topdir.exists():
        shutil.rmtree(topdir)
    for child in ("BUILD", "RPMS", "SOURCES", "SPECS", "SRPMS"):
        (topdir / child).mkdir(parents=True, exist_ok=True)

    rpm_ver, rpm_rel = rpm_version(version)
    source_root = output_root / f"scratchbird-{rpm_ver}"
    if source_root.exists():
        shutil.rmtree(source_root)
    shutil.copytree(payload_root, source_root)
    source_tar = topdir / "SOURCES" / f"scratchbird-{rpm_ver}.tar.gz"
    with tarfile.open(source_tar, "w:gz", format=tarfile.PAX_FORMAT) as archive:
        archive.add(
            source_root,
            arcname=f"scratchbird-{rpm_ver}",
            recursive=True,
            filter=normalize_tar_owner,
        )
    shutil.rmtree(source_root)

    spec = topdir / "SPECS" / "scratchbird.spec"
    rpm_requires = "".join(
        f"Requires: {dependency}\n" for dependency in RPM_RUNTIME_REQUIREMENTS
    )
    rpm_config_files = "\n".join(
        f"%config(noreplace) /{path.relative_to(payload_root).as_posix()}"
        for path in sorted(
            item
            for item in (payload_root / "etc" / "scratchbird").rglob("*")
            if item.is_file()
        )
    )
    spec.write_text(
        f"""Name: scratchbird
Version: {rpm_ver}
Release: {rpm_rel}%{{?dist}}
Summary: ScratchBird Convergent Data Engine pre-release build
License: MPL-2.0
URL: https://scratchbird.com
Source0: scratchbird-{rpm_ver}.tar.gz
{rpm_requires}

%description
ScratchBird Convergent Data Engine pre-release build.

%prep
%setup -q

%build

%install
rm -rf %{{buildroot}}
mkdir -p %{{buildroot}}
cp -a opt etc usr %{{buildroot}}/

%post
/usr/lib/scratchbird/scratchbird-system-install post-install --package-format rpm --package-version {rpm_ver}-{rpm_rel}

%preun
if [ "$1" -eq 0 ] && [ -x /usr/lib/scratchbird/scratchbird-system-install ]; then
  /usr/lib/scratchbird/scratchbird-system-install pre-remove --package-format rpm --package-version {rpm_ver}-{rpm_rel}
fi

%postun
if [ "$1" -eq 0 ]; then
  if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload >/dev/null 2>&1 || true
  fi
{LINUX_CONFIG_RESTORE_COMMANDS}
fi

%files
%defattr(-,root,root,-)
/opt/ScratchBird
%dir /etc/scratchbird
{rpm_config_files}
/usr/lib/scratchbird/scratchbird-system-install
/usr/lib/systemd/system/scratchbird-sbsrv.service
/usr/lib/sysusers.d/scratchbird.conf
/usr/lib/tmpfiles.d/scratchbird.conf
""",
        encoding="utf-8",
    )
    recipe = output_root / "scratchbird-linux-system.spec"
    shutil.copy2(spec, recipe)
    if not rpm_bin:
        if require_rpm:
            fail("rpmbuild_not_found")
        return [source_tar, spec]
    run([rpm_bin, "-bb", "--define", f"_topdir {topdir}", str(spec)], cwd=output_root)
    rpms = sorted((topdir / "RPMS").rglob("*.rpm"))
    copied: list[Path] = []
    for rpm in rpms:
        target = output_root / rpm.name
        shutil.copy2(rpm, target)
        copied.append(target)
    if copied:
        # The rpmbuild tree can contain multiple uncompressed copies of the
        # complete installation payload.  Only the verified RPM is a release
        # artifact once the native package build succeeds.
        shutil.rmtree(topdir)
        return copied
    if require_rpm:
        fail("rpmbuild_completed_without_rpm")
    return [source_tar, spec]


def make_aur(payload_root: Path, output_root: Path, version: str) -> Path:
    aur_root = output_root / "aur" / "scratchbird"
    if aur_root.exists():
        shutil.rmtree(aur_root)
    aur_root.mkdir(parents=True)
    source_name = f"scratchbird-{sanitize_version(version)}.tar.gz"
    source_path = aur_root / source_name
    with tarfile.open(source_path, "w:gz", format=tarfile.PAX_FORMAT) as archive:
        archive.add(
            payload_root,
            arcname=f"scratchbird-{sanitize_version(version)}",
            recursive=True,
            filter=normalize_tar_owner,
        )
    digest = sha256_file(source_path)
    aur_version = sanitize_version(version).replace("-", "_")
    backup_files = " ".join(
        repr(path.relative_to(payload_root).as_posix())
        for path in sorted(
            item
            for item in (payload_root / "etc" / "scratchbird").rglob("*")
            if item.is_file()
        )
    )
    (aur_root / "scratchbird.install").write_text(
        f"""post_install() {{
  /usr/lib/scratchbird/scratchbird-system-install post-install --package-format aur --package-version {aur_version}
}}

post_upgrade() {{
  post_install
}}

pre_remove() {{
  if [[ -x /usr/lib/scratchbird/scratchbird-system-install ]]; then
    /usr/lib/scratchbird/scratchbird-system-install pre-remove --package-format aur --package-version {aur_version}
  fi
}}

post_remove() {{
  if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload >/dev/null 2>&1 || true
  fi
{LINUX_CONFIG_RESTORE_COMMANDS}
}}
""",
        encoding="utf-8",
    )
    (aur_root / "PKGBUILD").write_text(
        f"""pkgname=scratchbird
pkgver={aur_version}
pkgrel=1
pkgdesc='ScratchBird Convergent Data Engine pre-release build'
arch=('x86_64')
url='https://scratchbird.com'
license=('MPL-2.0')
depends=({' '.join(repr(dependency) for dependency in AUR_RUNTIME_DEPENDENCIES)})
source=('{source_name}')
sha256sums=('{digest}')
install=scratchbird.install
backup=({backup_files})

package() {{
  cp -a "$srcdir"/scratchbird-{sanitize_version(version)}/opt "$pkgdir"/
  cp -a "$srcdir"/scratchbird-{sanitize_version(version)}/etc "$pkgdir"/
  cp -a "$srcdir"/scratchbird-{sanitize_version(version)}/usr "$pkgdir"/
  chown -R root:root "$pkgdir"
}}
""",
        encoding="utf-8",
    )
    bundle = output_root / f"scratchbird-aur-{sanitize_version(version)}.tar.gz"
    with tarfile.open(bundle, "w:gz", format=tarfile.PAX_FORMAT) as archive:
        archive.add(aur_root, arcname="scratchbird", recursive=True)
    # The outer AUR bundle contains the verified source archive. Keep the
    # source-tracked recipe/install hook as review evidence without retaining a
    # second full payload copy in the installer artifact directory.
    source_path.unlink()
    return bundle


def make_zip(payload_root: Path, output_root: Path, version: str) -> Path:
    output = output_root / f"scratchbird-windows-{sanitize_version(version)}.zip"
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in sorted(payload_root.rglob("*")):
            if path.is_file():
                archive.write(path, path.relative_to(payload_root).as_posix())
    return output


def macos_binary_candidates(payload_root: Path) -> list[Path]:
    candidates: list[Path] = []
    for root in (payload_root / "opt" / "ScratchBird" / "bin", payload_root / "opt" / "ScratchBird" / "lib"):
        if not root.exists():
            continue
        for path in sorted(item for item in root.rglob("*") if item.is_file()):
            if path.suffix in {".a", ".h", ".hpp", ".json", ".txt", ".md"}:
                continue
            if path.suffix in {".dylib", ".so"} or os.access(path, os.X_OK):
                candidates.append(path)
    return candidates


def write_macos_dynamic_library_audit(payload_root: Path, output_root: Path) -> Path:
    otool = shutil.which("otool")
    if not otool:
        fail("macos_otool_not_found")
    rows = []
    forbidden_fragments = (
        payload_root.as_posix(),
        "/build/",
        "build/public-release",
        "CMakeFiles",
    )
    for path in macos_binary_candidates(payload_root):
        output = run([otool, "-L", str(path)], cwd=payload_root)
        dependency_lines = output.splitlines()[1:]
        for fragment in forbidden_fragments:
            if any(fragment in line for line in dependency_lines):
                fail(f"macos_dylib_build_path_leak:{path.relative_to(payload_root).as_posix()}:{fragment}")
        rows.append(
            {
                "path": path.relative_to(payload_root).as_posix(),
                "otool_L": output.splitlines(),
                "status": "checked",
            }
        )
    if not rows:
        fail("macos_dynamic_library_candidates_missing")
    audit = {
        "schema_id": "scratchbird.macos_dynamic_library_audit.v1",
        "checks": [
            "otool -L",
            "no build-tree paths",
            "no staged payload root absolute paths",
            "system, @rpath, and externally declared Homebrew paths permitted",
        ],
        "dependency_policy": (
            "Homebrew LLVM remains an external QA runtime prerequisite and is "
            "loaded explicitly, so it is not visible in otool -L output."
        ),
        "external_runtime_prerequisites": MACOS_SUPPORT_MATRIX[
            "external_runtime_prerequisites"
        ],
        "rows": rows,
    }
    path = output_root / "MACOS_DYNAMIC_LIBRARY_AUDIT.json"
    path.write_text(json.dumps(audit, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return path


def maybe_sign_macos_payload(payload_root: Path) -> dict[str, Any]:
    signing_enabled = os.environ.get("SB_MACOS_RELEASE_SIGNING_ENABLED") == "true"
    signing_mode = os.environ.get("SB_MACOS_SIGNING_MODE", "qa-unsigned")
    state: dict[str, Any] = {
        "schema_id": "scratchbird.macos_signing_state.v1",
        "release_signing_enabled": signing_enabled,
        "signing_mode": signing_mode,
        "codesign": shutil.which("codesign") is not None,
        "spctl": shutil.which("spctl") is not None,
        "pkgutil": shutil.which("pkgutil") is not None,
        "notarization": "not_requested",
        "artifacts": [],
    }
    if not signing_enabled:
        state["status"] = "qa_unsigned_not_for_public_signed_release"
        return state
    identity = os.environ.get("SB_MACOS_DEVELOPER_ID_APPLICATION")
    if not identity:
        fail("macos_release_signing_enabled_without_application_identity")
    if not shutil.which("codesign"):
        fail("macos_release_signing_enabled_without_codesign")
    for path in macos_binary_candidates(payload_root):
        run(["codesign", "--force", "--options", "runtime", "--timestamp", "--sign", identity, str(path)], cwd=payload_root)
        verify_output = run(["codesign", "--verify", "--strict", "--verbose=2", str(path)], cwd=payload_root)
        state["artifacts"].append(
            {
                "path": path.relative_to(payload_root).as_posix(),
                "identity": identity,
                "verification": verify_output.splitlines(),
                "status": "signed",
            }
        )
    state["status"] = "payload_signed"
    return state


def write_macos_signing_state(output_root: Path, state: dict[str, Any]) -> Path:
    path = output_root / "MACOS_SIGNING_STATE.json"
    path.write_text(json.dumps(state, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return path


def make_macos_pkg(
    payload_root: Path,
    output_root: Path,
    version: str,
    signing_state: dict[str, Any],
    scripts_root: Path,
) -> Path:
    pkgbuild = shutil.which("pkgbuild")
    if not pkgbuild:
        fail("pkgbuild_not_found")
    package = output_root / f"scratchbird-macos-{sanitize_version(version)}.pkg"
    command = [
        pkgbuild,
        "--root",
        str(payload_root),
        "--identifier",
        "com.scratchbird.cde",
        "--version",
        sanitize_version(version),
        "--ownership",
        "recommended",
        "--scripts",
        str(scripts_root),
    ]
    installer_identity = os.environ.get("SB_MACOS_DEVELOPER_ID_INSTALLER")
    if signing_state.get("release_signing_enabled"):
        if not installer_identity:
            fail("macos_release_signing_enabled_without_installer_identity")
        command.extend(["--sign", installer_identity])
    command.append(str(package))
    run(command, cwd=output_root)
    pkgutil = shutil.which("pkgutil")
    spctl = shutil.which("spctl")
    checks: dict[str, Any] = {
        "pkgutil_check_signature": "not_available",
        "spctl_assess": "not_requested",
    }
    if pkgutil:
        result = subprocess.run([pkgutil, "--check-signature", str(package)], cwd=output_root, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        checks["pkgutil_check_signature"] = result.stdout.splitlines()
        if signing_state.get("release_signing_enabled") and result.returncode != 0:
            print(result.stdout, end="")
            fail("macos_pkg_signature_check_failed")
    if spctl and signing_state.get("release_signing_enabled"):
        result = subprocess.run([spctl, "--assess", "--type", "install", "--verbose=2", str(package)], cwd=output_root, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        checks["spctl_assess"] = result.stdout.splitlines()
        if result.returncode != 0:
            print(result.stdout, end="")
            fail("macos_pkg_gatekeeper_assess_failed")
    signing_state["package"] = {"path": package.name, **checks}
    return package


def xml_id(prefix: str, value: str) -> str:
    digest = hashlib.sha1(value.encode("utf-8")).hexdigest()[:16]
    safe = re.sub(r"[^A-Za-z0-9_]", "_", value)
    safe = safe[:42].strip("_") or "item"
    return f"{prefix}_{safe}_{digest}"


def wix_directory_xml(root: Path, dir_path: Path, component_refs: list[str]) -> str:
    rel = dir_path.relative_to(root).as_posix() if dir_path != root else ""
    lines: list[str] = []
    for child_dir in sorted(item for item in dir_path.iterdir() if item.is_dir()):
        dir_id = xml_id("dir", child_dir.relative_to(root).as_posix())
        lines.append(f'<Directory Id="{dir_id}" Name="{escape(child_dir.name)}">')
        for file_path in sorted(item for item in child_dir.iterdir() if item.is_file()):
            rel_file = file_path.relative_to(root).as_posix()
            comp_id = xml_id("cmp", rel_file)
            file_id = xml_id("fil", rel_file)
            component_refs.append(comp_id)
            lines.append(f'<Component Id="{comp_id}" Guid="*"><File Id="{file_id}" Source="{escape(str(file_path))}" KeyPath="yes" /></Component>')
        lines.append(wix_directory_xml(root, child_dir, component_refs))
        lines.append("</Directory>")
    if rel == "":
        for file_path in sorted(item for item in dir_path.iterdir() if item.is_file()):
            rel_file = file_path.relative_to(root).as_posix()
            comp_id = xml_id("cmp", rel_file)
            file_id = xml_id("fil", rel_file)
            component_refs.append(comp_id)
            lines.append(f'<Component Id="{comp_id}" Guid="*"><File Id="{file_id}" Source="{escape(str(file_path))}" KeyPath="yes" /></Component>')
    return "\n".join(lines)


def materialize_windows_wix_lifecycle(
    output_root: Path,
    version: str,
) -> Path:
    source = WINDOWS_ASSET_ROOT / WINDOWS_WIX_LIFECYCLE_TEMPLATE
    if not source.is_file():
        fail(f"windows_wix_lifecycle_asset_missing:{source}")
    text = source.read_text(encoding="utf-8")
    if text.count("@SCRATCHBIRD_VERSION@") != 2:
        fail("windows_wix_lifecycle_version_token_mismatch")
    target = output_root / "scratchbird-windows-lifecycle.wxs"
    target.write_text(
        text.replace("@SCRATCHBIRD_VERSION@", windows_msi_version(version)),
        encoding="utf-8",
    )
    return target


def make_wix_msi(payload_root: Path, output_root: Path, version: str, require_msi: bool) -> list[Path]:
    wix_bin = shutil.which("wix")
    wxs = output_root / "scratchbird.wxs"
    lifecycle_wxs = materialize_windows_wix_lifecycle(output_root, version)
    component_refs: list[str] = []
    directory_xml = wix_directory_xml(payload_root, payload_root, component_refs)
    refs_xml = "\n".join(f'<ComponentRef Id="{ref}" />' for ref in component_refs)
    wxs.write_text(
        f"""<?xml version="1.0" encoding="utf-8"?>
<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs">
  <Package Name="{PRODUCT_NAME}" Manufacturer="{MANUFACTURER}" Version="{windows_msi_version(version)}" UpgradeCode="{WINDOWS_UPGRADE_CODE}" Scope="perMachine">
    <MajorUpgrade DowngradeErrorMessage="A newer ScratchBird build is already installed." />
    <MediaTemplate EmbedCab="yes" />
    <StandardDirectory Id="ProgramFiles64Folder">
      <Directory Id="INSTALLFOLDER" Name="ScratchBird">
{directory_xml}
      </Directory>
    </StandardDirectory>
    <Feature Id="Main" Title="ScratchBird" Level="1">
{refs_xml}
    </Feature>
  </Package>
</Wix>
""",
        encoding="utf-8",
    )
    if not wix_bin:
        if require_msi:
            fail("wix_not_found")
        return [wxs, lifecycle_wxs]
    msi = output_root / f"scratchbird-windows-{sanitize_version(version)}.msi"
    run(
        [
            wix_bin,
            "build",
            str(wxs),
            str(lifecycle_wxs),
            "-arch",
            "x64",
            "-ext",
            "WixToolset.Util.wixext",
            "-o",
            str(msi),
        ],
        cwd=output_root,
    )
    return [msi, wxs, lifecycle_wxs]


def write_linux_system_package_evidence(
    output_root: Path,
    version: str,
    build_id: str | None,
) -> Path:
    evidence = {
        "schema_id": "scratchbird.linux_system_package_evidence.v1",
        "version": version,
        "build_id": build_id,
        "native_default_port": 3092,
        "portable_tar_policy": "payload_only_no_host_lifecycle",
        "system_package_formats": ["deb", "rpm", "aur"],
        "service": {
            "unit": LINUX_SERVICE_NAME,
            "default_enablement": "disabled",
            "default_activity": "not_started",
            "top_level_process": "SBsrv",
            "child_process_ownership": "SBsrv_to_SBgate_to_standalone_SBParser",
        },
        "os_identity": {
            "user": "scratchbird",
            "group": "scratchbird",
            "login": "forbidden",
            "service_authority_scope": (
                "filesystem_directory_and_process_execution_only_"
                "no_database_or_security_authority"
            ),
            "service_effective_group_policy": (
                "only_scratchbird_effective_group"
            ),
            "create_time_os_authorization": "root_only",
            "human_service_group_membership_mutation": False,
        },
        "lifecycle_assets": sorted(LINUX_SYSTEM_ASSETS.values()),
        "database_files_created": False,
        "security_sidecars_created": False,
        "verification": "pending_linux_system_package_smoke",
    }
    path = output_root / "LINUX_SYSTEM_PACKAGE_EVIDENCE.json"
    path.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    path.chmod(0o644)
    return path


def write_windows_system_package_evidence(
    output_root: Path,
    version: str,
    build_id: str | None,
) -> Path:
    evidence = {
        "schema_id": "scratchbird.windows_system_package_evidence.v1",
        "version": version,
        "build_id": build_id,
        "distribution_profile": "native-sbsql-only",
        "native_default_port": 3092,
        "portable_zip_policy": "payload_only_no_host_lifecycle",
        "system_package_format": "msi",
        "configuration": {
            "live_root": WINDOWS_CONFIG_ROOT,
            "pristine_defaults": (
                r"%ProgramFiles%\ScratchBird\share\scratchbird\config-defaults"
            ),
            "native_config_set": list(WINDOWS_NATIVE_CONFIGS),
            "copy_policy": "copy_only_when_missing_materialize_known_folder_paths",
        },
        "service": {
            "name": "scratchbird",
            "account": r"NT SERVICE\scratchbird",
            "account_kind": "restricted_managed_virtual_service_account",
            "default_start_type": "manual",
            "default_activity": "stopped",
            "fresh_install_creates_missing_service": True,
            "creation_mechanism": (
                "elevated_deferred_msi_lifecycle_helper_Ensure-SBsrvService"
            ),
            "service_sid_type": "restricted",
            "top_level_process": "SBsrv",
            "child_process_ownership": (
                "SBsrv_to_shared_SBgate_to_standalone_SBParser"
            ),
            "parser_or_listener_services_installed": False,
            "manager_service_installed": False,
            "scm_runtime_handoff": (
                "native_SBsrv_ServiceMain_in_process_no_wrapper"
            ),
            "scm_running_gate": "SBPS_and_shared_SBgate_management_ready",
            "scm_stop_path": "in_process_parser_server_stop_api",
            "scm_runtime_proof": (
                "focused_contract_green_hosted_configured_database_start_stop_pending"
            ),
        },
        "os_identity": {
            "filesystem_operations_group": "ScratchBird",
            "filesystem_operations_group_namespace": "local_SAM",
            "service_account_namespace": "NT SERVICE",
            "service_account_leaf_name": "scratchbird",
            "service_password": "none",
            "local_sam_service_user_created": False,
            "administrator_group_membership": False,
            "local_sam_group_membership": False,
            "service_authority_scope": (
                "filesystem_directory_and_process_execution_only_"
                "no_database_or_security_authority"
            ),
            "create_time_os_authorization": "administrator_only",
            "human_service_group_membership_mutation": False,
        },
        "lifecycle_assets": sorted(WINDOWS_SYSTEM_ASSETS.values()),
        "database_files_created": False,
        "security_sidecars_created": False,
        "upgrade_preserves_operator_config_data_and_service_state": True,
        "uninstall_preserves_operator_config_data_logs_and_group": True,
        "verification": "pending_windows_actual_msi_install_smoke",
    }
    path = output_root / "WINDOWS_SYSTEM_PACKAGE_EVIDENCE.json"
    path.write_text(
        json.dumps(evidence, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return path


def write_macos_system_package_evidence(
    output_root: Path,
    version: str,
    build_id: str | None,
) -> Path:
    evidence = {
        "schema_id": "scratchbird.macos_system_package_evidence.v1",
        "version": version,
        "build_id": build_id,
        "distribution_profile": "native-sbsql-only",
        "native_default_port": 3092,
        "portable_tar_policy": "payload_only_with_portable_etc_layout",
        "system_package_format": "pkg",
        "configuration": {
            "live_root": MACOS_CONFIG_ROOT,
            "pristine_defaults": f"/{MACOS_CONFIG_DEFAULTS_REL}",
            "copy_policy": "copy_only_when_missing",
            "duplicate_etc_live_config": False,
        },
        "service": {
            "launchd_labels": [
                "com.scratchbird.sbsrv",
                "com.scratchbird.sbmgr",
            ],
            "default_enablement": "disabled",
            "default_activity": "not_started",
            "run_at_load": False,
            "top_level_process": "SBsrv",
            "child_process_ownership": (
                "SBsrv_to_shared_SBgate_to_standalone_SBParser"
            ),
        },
        "os_identity": {
            "user": "scratchbird",
            "group": "scratchbird",
            "login": "forbidden",
            "hidden": True,
            "administrator_group_membership": False,
            "service_authority_scope": (
                "filesystem_directory_and_process_execution_only_"
                "no_database_or_security_authority"
            ),
            "uid_policy": "first_locally_unused_uid_501_through_59999",
            "resolved_effective_group_policy": (
                "primary_scratchbird_plus_macos_implicit_gid_12_and_61_only"
            ),
            "create_time_os_authorization": "root_only",
            "human_service_group_membership_mutation": False,
        },
        "lifecycle_helper": (
            "/opt/ScratchBird/libexec/scratchbird-macos-system-install"
        ),
        "database_files_created": False,
        "security_sidecars_created": False,
        "verification": "pending_macos_system_package_smoke",
    }
    path = output_root / "MACOS_SYSTEM_PACKAGE_EVIDENCE.json"
    path.write_text(
        json.dumps(evidence, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    path.chmod(0o644)
    return path


def write_artifact_manifest(output_root: Path, platform: str, version: str, build_id: str | None) -> Path:
    rows = []
    for path in sorted(item for item in output_root.rglob("*") if item.is_file()):
        if path.name == MANIFEST_NAME:
            continue
        rel = path.relative_to(output_root).as_posix()
        rows.append({"path": rel, "bytes": path.stat().st_size, "sha256": sha256_file(path)})
    manifest = {
        "schema_id": "scratchbird.installer_artifact_manifest.v1",
        "generated_at_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
        "platform": platform,
        "version": version,
        "build_id": build_id,
        "artifacts": rows,
    }
    if platform == "windows":
        manifest["windows"] = {
            "system_package_evidence_file": (
                "WINDOWS_SYSTEM_PACKAGE_EVIDENCE.json"
            ),
            "service_name": "scratchbird",
            "service_account": r"NT SERVICE\scratchbird",
            "native_default_port": 3092,
            "actual_install_smoke_required": True,
        }
    if platform == "macos":
        manifest["macos"] = {
            "support_matrix": MACOS_SUPPORT_MATRIX,
            "signing_state_file": "MACOS_SIGNING_STATE.json",
            "dynamic_library_audit_file": "MACOS_DYNAMIC_LIBRARY_AUDIT.json",
            "system_package_launchd_manifest_file": (
                "opt/ScratchBird/share/scratchbird/release/"
                "MACOS_LAUNCHD_MANIFEST.json"
            ),
            "system_package_evidence_file": "MACOS_SYSTEM_PACKAGE_EVIDENCE.json",
        }
    path = output_root / MANIFEST_NAME
    path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    (output_root / "SHA256SUMS").write_text(
        "\n".join(f"{row['sha256']}  {row['path']}" for row in rows) + "\n",
        encoding="utf-8",
    )
    scan_private_text(output_root)
    return path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifact-root", type=Path, required=True)
    parser.add_argument("--platform", choices=("linux", "windows", "macos"), required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--version", default="0.0.0-nightly")
    parser.add_argument("--build-id")
    parser.add_argument("--require-rpm", action="store_true")
    parser.add_argument("--require-msi", action="store_true")
    parser.add_argument("--require-native-only", action="store_true")
    args = parser.parse_args()

    artifact_root = args.artifact_root.resolve()
    output_root = args.output_root.resolve()
    version = sanitize_version(args.version)
    require_staged_output(
        artifact_root,
        args.platform,
        require_native_only=args.require_native_only,
    )
    if "packaging" in artifact_root.parts:
        fail(f"packaging_input_forbidden:{artifact_root}")
    if output_root.exists():
        shutil.rmtree(output_root)
    output_root.mkdir(parents=True)

    with tempfile.TemporaryDirectory(prefix="scratchbird-installer-") as temp_name:
        payload_root = Path(temp_name) / "payload"
        stage_install_tree(artifact_root, payload_root, args.platform, version, args.build_id)
        built: list[Path] = []
        if args.platform == "linux":
            system_payload_root = Path(temp_name) / "system-payload"
            built.append(make_tarball(payload_root, output_root, version, "linux"))
            stage_linux_system_install_tree(
                payload_root,
                system_payload_root,
                version,
                args.build_id,
            )
            built.append(make_deb(system_payload_root, output_root, version))
            built.extend(make_rpm(system_payload_root, output_root, version, args.require_rpm))
            built.append(make_aur(system_payload_root, output_root, version))
            write_linux_system_package_evidence(output_root, version, args.build_id)
        elif args.platform == "windows":
            system_payload_root = Path(temp_name) / "windows-system-payload"
            built.append(make_zip(payload_root, output_root, version))
            stage_windows_system_install_tree(
                payload_root,
                system_payload_root,
                version,
                args.build_id,
            )
            built.extend(
                make_wix_msi(
                    system_payload_root,
                    output_root,
                    version,
                    args.require_msi,
                )
            )
            write_windows_system_package_evidence(
                output_root,
                version,
                args.build_id,
            )
        else:
            system_payload_root = Path(temp_name) / "macos-system-payload"
            pkg_scripts_root = Path(temp_name) / "macos-pkg-scripts"
            signing_state = maybe_sign_macos_payload(payload_root)
            write_macos_dynamic_library_audit(payload_root, output_root)
            built.append(make_tarball(payload_root, output_root, version, "macos"))
            stage_macos_system_install_tree(
                payload_root,
                system_payload_root,
                version,
                args.build_id,
            )
            materialize_macos_pkg_scripts(pkg_scripts_root, version)
            built.append(
                make_macos_pkg(
                    system_payload_root,
                    output_root,
                    version,
                    signing_state,
                    pkg_scripts_root,
                )
            )
            write_macos_signing_state(output_root, signing_state)
            write_macos_system_package_evidence(
                output_root,
                version,
                args.build_id,
            )
        manifest = write_artifact_manifest(output_root, args.platform, version, args.build_id)
    print(f"build_installers=passed:{manifest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
