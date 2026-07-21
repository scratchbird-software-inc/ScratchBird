#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Shared fail-closed native-server admission checks for installer artifacts.

The public rolling native release may contain the server runtime only.  An
installer manifest's admission block is therefore merely the starting point:
every portable payload is unpacked and verified against its bound profile
digest and exact installed-file inventory.  Public publication deliberately
does not admit system-installer formats (DEB, AUR, RPM, PKG, or MSI).
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
from typing import Any
import zipfile


TOOL_ROOT = Path(__file__).resolve().parent
REPO_ROOT = TOOL_ROOT.parents[2]
NATIVE_PAYLOAD_VERIFIER = (
    REPO_ROOT / "project" / "tools" / "release" / "verify_native_installed_payload.py"
)
NATIVE_PROFILE_ARCHIVE_PATH = Path(
    "opt/ScratchBird/share/scratchbird/release/NATIVE_RELEASE_PROFILE.json"
)
INSTALL_MANIFEST_ARCHIVE_PATH = Path(
    "opt/ScratchBird/share/scratchbird/release/INSTALL_MANIFEST.json"
)
INSTALL_SHA256SUMS_ARCHIVE_PATH = Path(
    "opt/ScratchBird/share/scratchbird/release/SHA256SUMS"
)
NATIVE_SERVER_ADMISSION_SCHEMA = "scratchbird.installer_native_server_admission.v1"
NATIVE_SERVER_ADMISSION = {
    "schema_id": NATIVE_SERVER_ADMISSION_SCHEMA,
    "distribution_surface": "scratchbird_native_no_emulation",
    "admission_controller": "native_server_only",
    "client_artifacts_permitted": False,
    "admitted_driver_adaptor_mcp_components": [],
    "dbeaver_hard_excluded": True,
}
INVENTORY_SCHEMA = "scratchbird.native_server_payload_inventory.v1"
NATIVE_POLICY = "native_server_only_no_client_driver_adaptor_mcp_dbeaver"

MAX_MEMBER_COUNT = 100_000
MAX_MEMBER_BYTES = 16 * 1024 * 1024 * 1024
MAX_TOTAL_BYTES = 32 * 1024 * 1024 * 1024
MAX_JSON_BYTES = 16 * 1024 * 1024
MAX_PACKAGE_CONTROL_TEXT_BYTES = 2 * 1024 * 1024

# A recursively verified payload is still not a native-server payload when it
# installs a neutral-named binary outside the declared runtime layout.  The
# runtime verifier owns the exact bin/lib/config/share inventories; this
# boundary owns the enclosing install-root allowlist so a package cannot place
# an otherwise unnamed client component under usr/, var/, or a second opt/
# tree.
PORTABLE_RUNTIME_PREFIXES = (
    "opt/ScratchBird/bin/",
    "opt/ScratchBird/lib/",
    "opt/ScratchBird/share/scratchbird/",
    "etc/scratchbird/",
)
LINUX_SYSTEM_EXACT_FILES = frozenset(
    {
        "usr/lib/scratchbird/scratchbird-system-install",
        "usr/lib/systemd/system/scratchbird-sbsrv.service",
        "usr/lib/sysusers.d/scratchbird.conf",
        "usr/lib/tmpfiles.d/scratchbird.conf",
    }
)
MACOS_SYSTEM_EXACT_FILES = frozenset(
    {
        "opt/ScratchBird/libexec/scratchbird-macos-system-install",
        "Library/LaunchDaemons/com.scratchbird.sbsrv.plist",
        "Library/LaunchDaemons/com.scratchbird.sbmgr.plist",
    }
)

# Directly readable package formats are accepted only after their complete
# control surface has been read as text.  These patterns reject the ways a
# package recipe/script could fetch, decode, or execute an undeclared client
# after the data archive itself has passed the payload allowlist.
PACKAGE_CONTROL_IDENTITY = re.compile(
    r"(?i)(?:"
    r"dbeaver|scratchbird[_-]?(?:client|odbc|jdbc|r2dbc|mojo[_-]?client[_-]?bridge|ai[_-]?mcp)"
    r"|\b(?:jdbc|odbc|r2dbc|driver(?:s)?|adapter(?:s)?|adaptor(?:s)?|mcp(?:s)?)\b"
    r")"
)
PACKAGE_CONTROL_EXECUTION = re.compile(
    r"(?im)(?:"
    r"\b(?:curl|wget|ftp|tftp|scp|sftp|ssh|git|base64|uudecode|xxd|openssl|python(?:3)?|perl|ruby|node|php|lua|"
    r"powershell|pwsh|cscript|wscript|certutil|bitsadmin|mshta|dd|tee)\b"
    r"|\beval\b|^\s*(?:source|\.)\s+"
    r")"
)

# All public driver/adaptor/MCP implementations remain unadmitted.  Match both
# explicit public identities and generic component boundaries, so a renamed
# binary cannot hide underneath a neutral directory.
CLIENT_IDENTITY_MARKERS = (
    "dbeaver",
    "scratchbird_client",
    "scratchbird-client",
    "scratchbird_odbc",
    "scratchbird-odbc",
    "scratchbird_jdbc",
    "scratchbird-jdbc",
    "scratchbird_r2dbc",
    "scratchbird-r2dbc",
    "scratchbird_mojo_client_bridge",
    "scratchbird-mojo-client-bridge",
    "scratchbird_ai_mcp",
    "scratchbird-ai-mcp",
    "libscratchbird_client",
    "libscratchbird_odbc",
    "libscratchbird_jdbc",
    "libscratchbird_r2dbc",
)
CLIENT_PATH_COMPONENTS = frozenset(
    {
        "client",
        "clients",
        "driver",
        "drivers",
        "adapter",
        "adapters",
        "adaptor",
        "adaptors",
        "mcp",
        "mcps",
        "dbeaver",
        "jdbc",
        "odbc",
        "r2dbc",
    }
)
# This is a server-runtime example script shipped in the canonical resource
# tree, not an installable driver/adaptor/MCP.  Its filename documents a
# driver-routing smoke scenario and therefore contains the generic word
# ``driver``.  Keep the exception path-complete and case-normalized: no other
# driver-named file or directory is admitted by this boundary.
APPROVED_NONPAYLOAD_CLIENT_MARKER_PATHS = frozenset(
    {
        "opt/scratchbird/share/scratchbird/examples/core_beta_qa/"
        "driver_route_smoke.sh",
    }
)
REQUIRED_NATIVE_COMPONENTS = (
    "SBmgr",
    "SBgate",
    "SBParser",
    "SBsrv",
    "SBsql",
    "SBadm",
    "SBbak",
    "SBsec",
    "SBdoc",
    "SBcop",
)
REQUIRED_NATIVE_CONFIGS = (
    "SBsrv.conf",
    "SBgate.conf",
    "SBmgr.conf",
    "SBParser.conf",
    "SBbootstrap.profile",
)


class NativeAdmissionError(RuntimeError):
    """Raised when a native installer proof is absent or invalid."""


def fail(message: str) -> None:
    raise NativeAdmissionError(message)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def canonical_json_sha256(value: object) -> str:
    return sha256_bytes(
        json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    )


def require_sha256(value: object, context: str) -> str:
    if not isinstance(value, str) or re.fullmatch(r"[0-9a-f]{64}", value) is None:
        fail(f"native_admission_sha256_invalid:{context}")
    return value


def load_json(path: Path, context: str) -> dict[str, Any]:
    try:
        if path.stat().st_size > MAX_JSON_BYTES:
            fail(f"native_admission_json_too_large:{context}:{path.name}")
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        fail(f"native_admission_json_invalid:{context}:{path.name}:{exc}")
    if not isinstance(value, dict):
        fail(f"native_admission_json_not_object:{context}:{path.name}")
    return value


def client_payload_identity(value: str) -> bool:
    lowered = value.casefold().replace("\\", "/")
    if lowered in APPROVED_NONPAYLOAD_CLIENT_MARKER_PATHS:
        return False
    if any(marker in lowered for marker in CLIENT_IDENTITY_MARKERS):
        return True
    components = [part for part in re.split(r"[./_-]+", lowered) if part]
    return any(component in CLIENT_PATH_COMPONENTS for component in components)


def reject_client_payload_identity(value: str, context: str) -> None:
    if client_payload_identity(value):
        fail(f"native_admission_client_payload_forbidden:{context}:{value}")


def safe_relative_path(value: str, context: str) -> Path:
    path = Path(value)
    if (
        not value
        or path.is_absolute()
        or "\\" in value
        or re.match(r"^[A-Za-z]:", value) is not None
        or any(part in {"", ".", ".."} for part in path.parts)
    ):
        fail(f"native_admission_unsafe_path:{context}:{value}")
    normalized = path.as_posix()
    reject_client_payload_identity(normalized, context)
    return path


def require_native_server_admission(value: object, context: str) -> tuple[dict[str, Any], str]:
    if not isinstance(value, dict):
        fail(f"native_server_admission_missing:{context}")
    expected_keys = set(NATIVE_SERVER_ADMISSION) | {"native_release_profile_sha256"}
    if set(value) != expected_keys:
        fail(f"native_server_admission_fields_invalid:{context}")
    for key, expected in NATIVE_SERVER_ADMISSION.items():
        if value.get(key) != expected:
            fail(f"native_server_admission_invalid:{context}:{key}")
    digest = require_sha256(value.get("native_release_profile_sha256"), f"{context}:profile")
    return dict(value), digest


def _write_bytes(target: Path, source: Any, size: int, context: str) -> None:
    if size < 0 or size > MAX_MEMBER_BYTES:
        fail(f"native_admission_member_size_invalid:{context}:{size}")
    target.parent.mkdir(parents=True, exist_ok=True)
    with target.open("wb") as handle:
        shutil.copyfileobj(source, handle, length=1024 * 1024)
    if target.stat().st_size != size:
        fail(f"native_admission_member_size_mismatch:{context}")


def _archive_member_path(raw: str, archive_name: str) -> tuple[Path, str]:
    normalized = raw.rstrip("/")
    if not normalized:
        return Path("."), ""
    path = safe_relative_path(normalized, f"archive_path:{archive_name}")
    return path, path.as_posix()


def extract_tar_gz(archive_path: Path, target: Path) -> None:
    names: set[str] = set()
    total_size = 0
    try:
        with tarfile.open(archive_path, "r:gz") as archive:
            for member_count, member in enumerate(archive, start=1):
                if member_count > MAX_MEMBER_COUNT:
                    fail(f"native_admission_member_limit:{archive_path.name}")
                relative, rel = _archive_member_path(member.name, archive_path.name)
                if not rel:
                    continue
                if rel in names:
                    fail(f"native_admission_archive_duplicate:{archive_path.name}:{rel}")
                names.add(rel)
                if member.issym() or member.islnk() or member.isdev() or member.isfifo():
                    fail(f"native_admission_archive_special_entry:{archive_path.name}:{rel}")
                if member.isdir():
                    (target / relative).mkdir(parents=True, exist_ok=True)
                    continue
                if not member.isfile():
                    fail(f"native_admission_archive_member_type:{archive_path.name}:{rel}")
                total_size += member.size
                if total_size > MAX_TOTAL_BYTES:
                    fail(f"native_admission_archive_total_limit:{archive_path.name}")
                source = archive.extractfile(member)
                if source is None:
                    fail(f"native_admission_archive_member_unreadable:{archive_path.name}:{rel}")
                _write_bytes(target / relative, source, member.size, f"{archive_path.name}:{rel}")
    except (OSError, tarfile.TarError) as exc:
        fail(f"native_admission_tar_invalid:{archive_path.name}:{exc}")


def extract_zip(archive_path: Path, target: Path) -> None:
    names: set[str] = set()
    total_size = 0
    try:
        with zipfile.ZipFile(archive_path) as archive:
            for member_count, member in enumerate(archive.infolist(), start=1):
                if member_count > MAX_MEMBER_COUNT:
                    fail(f"native_admission_member_limit:{archive_path.name}")
                relative, rel = _archive_member_path(member.filename, archive_path.name)
                if not rel:
                    continue
                if rel in names:
                    fail(f"native_admission_archive_duplicate:{archive_path.name}:{rel}")
                names.add(rel)
                mode = (member.external_attr >> 16) & 0o170000
                if mode == stat.S_IFLNK:
                    fail(f"native_admission_archive_special_entry:{archive_path.name}:{rel}")
                if member.is_dir():
                    (target / relative).mkdir(parents=True, exist_ok=True)
                    continue
                total_size += member.file_size
                if total_size > MAX_TOTAL_BYTES:
                    fail(f"native_admission_archive_total_limit:{archive_path.name}")
                with archive.open(member) as source:
                    _write_bytes(target / relative, source, member.file_size, f"{archive_path.name}:{rel}")
    except (OSError, RuntimeError, zipfile.BadZipFile) as exc:
        fail(f"native_admission_zip_invalid:{archive_path.name}:{exc}")


def scan_native_only_tree(root: Path, context: str) -> None:
    if not root.is_dir() or root.is_symlink():
        fail(f"native_admission_payload_root_invalid:{context}")
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root).as_posix()
        safe_relative_path(relative, f"payload_path:{context}")
        if path.is_symlink():
            fail(f"native_admission_payload_symlink:{context}:{relative}")
        if not path.is_dir() and not path.is_file():
            fail(f"native_admission_payload_special_entry:{context}:{relative}")


def _path_in_prefixes(relative: str, prefixes: tuple[str, ...]) -> bool:
    return any(relative.startswith(prefix) for prefix in prefixes)


def _directory_allowed(relative: str, layout: str) -> bool:
    """Return whether an extracted directory belongs to one admitted tree."""

    portable = {
        "opt",
        "opt/ScratchBird",
        "opt/ScratchBird/bin",
        "opt/ScratchBird/lib",
        "opt/ScratchBird/share",
        "opt/ScratchBird/share/scratchbird",
        "etc",
        "etc/scratchbird",
    }
    if layout in {"portable", "linux_system"} and (
        relative in portable
        or relative.startswith("opt/ScratchBird/share/scratchbird/")
        or relative.startswith("etc/scratchbird/")
    ):
        return True
    if layout == "macos_system":
        macos_portable = portable - {"etc", "etc/scratchbird"}
        return (
            relative in macos_portable
            or relative.startswith("opt/ScratchBird/share/scratchbird/")
            or relative in {"opt/ScratchBird/libexec", "Library", "Library/LaunchDaemons"}
        )
    if layout == "linux_system":
        linux_system = {
            "usr",
            "usr/lib",
            "usr/lib/scratchbird",
            "usr/lib/systemd",
            "usr/lib/systemd/system",
            "usr/lib/sysusers.d",
            "usr/lib/tmpfiles.d",
        }
        return relative in linux_system
    return False


def validate_payload_layout(root: Path, layout: str, context: str) -> None:
    """Reject every installed file outside the declared native-server tree.

    The lower-level native payload verifier checks the exact executable,
    library, resource, and configuration inventories.  This separate check
    closes the remaining package-manager gap: a neutral name such as
    ``usr/lib/libfoo.so`` is not a permitted payload simply because it avoids
    a driver/client marker.
    """

    scan_native_only_tree(root, f"layout:{context}")
    if layout not in {"portable", "linux_system", "macos_system"}:
        fail(f"native_admission_payload_layout_invalid:{context}:{layout}")
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root).as_posix()
        if path.is_dir():
            if not _directory_allowed(relative, layout):
                fail(
                    "native_admission_payload_layout_directory_forbidden:"
                    f"{context}:{layout}:{relative}"
                )
            continue
        allowed = _path_in_prefixes(relative, PORTABLE_RUNTIME_PREFIXES)
        if layout == "linux_system":
            allowed = allowed or relative in LINUX_SYSTEM_EXACT_FILES
        elif layout == "macos_system":
            allowed = (
                _path_in_prefixes(
                    relative,
                    (
                        "opt/ScratchBird/bin/",
                        "opt/ScratchBird/lib/",
                        "opt/ScratchBird/share/scratchbird/",
                    ),
                )
                or relative in MACOS_SYSTEM_EXACT_FILES
            )
        if not allowed:
            fail(
                "native_admission_payload_layout_forbidden:"
                f"{context}:{layout}:{relative}"
            )


def _parse_payload_sha256sums(path: Path, context: str) -> dict[str, str]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeDecodeError) as exc:
        fail(f"native_admission_payload_sha256sums_invalid:{context}:{exc}")
    rows: dict[str, str] = {}
    for line in lines:
        match = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
        if match is None:
            fail(f"native_admission_payload_sha256sums_row_invalid:{context}:{line}")
        digest, raw_path = match.groups()
        relative = safe_relative_path(raw_path, f"payload_sha256sums:{context}")
        rel = relative.as_posix()
        if rel in rows:
            fail(f"native_admission_payload_sha256sums_duplicate:{context}:{rel}")
        rows[rel] = digest
    if not rows:
        fail(f"native_admission_payload_sha256sums_empty:{context}")
    return rows


def verify_declared_installed_payload(root: Path, platform: str, context: str) -> None:
    """Bind the full extracted tree to its installer payload inventory.

    Every generated install tree carries an ``INSTALL_MANIFEST.json`` and a
    matching ``SHA256SUMS``.  Treating those as an exact recursive inventory
    means a neutral file under ``usr/lib`` or a hidden package subtree cannot
    be ignored merely because it was not named like a driver.  The runtime
    profile/layout checks that follow constrain what this self-description is
    allowed to describe.
    """

    manifest_path = root / INSTALL_MANIFEST_ARCHIVE_PATH
    sums_path = root / INSTALL_SHA256SUMS_ARCHIVE_PATH
    if not manifest_path.is_file() or manifest_path.is_symlink():
        fail(f"native_admission_payload_install_manifest_missing:{context}")
    if not sums_path.is_file() or sums_path.is_symlink():
        fail(f"native_admission_payload_sha256sums_missing:{context}")
    manifest = load_json(manifest_path, f"payload_install_manifest:{context}")
    if manifest.get("schema_id") != "scratchbird.installer_payload_manifest.v1":
        fail(f"native_admission_payload_install_manifest_schema:{context}")
    if manifest.get("platform") != platform:
        fail(f"native_admission_payload_install_manifest_platform:{context}")
    rows = manifest.get("files")
    if not isinstance(rows, list) or not rows:
        fail(f"native_admission_payload_install_manifest_files:{context}")
    declared: dict[str, tuple[int, str]] = {}
    for row in rows:
        if not isinstance(row, dict) or set(row) != {"path", "bytes", "sha256"}:
            fail(f"native_admission_payload_install_manifest_row:{context}")
        raw_path = row.get("path")
        if not isinstance(raw_path, str):
            fail(f"native_admission_payload_install_manifest_path:{context}")
        relative = safe_relative_path(raw_path, f"payload_install_manifest:{context}")
        rel = relative.as_posix()
        size = row.get("bytes")
        if not isinstance(size, int) or size < 0:
            fail(f"native_admission_payload_install_manifest_size:{context}:{rel}")
        digest = require_sha256(row.get("sha256"), f"payload_install_manifest:{context}:{rel}")
        if rel in declared:
            fail(f"native_admission_payload_install_manifest_duplicate:{context}:{rel}")
        declared[rel] = (size, digest)

    actual_files: dict[str, Path] = {}
    for path in sorted(item for item in root.rglob("*") if item.is_file()):
        rel = path.relative_to(root).as_posix()
        actual_files[rel] = path
    metadata = {
        INSTALL_MANIFEST_ARCHIVE_PATH.as_posix(),
        INSTALL_SHA256SUMS_ARCHIVE_PATH.as_posix(),
    }
    expected = set(declared) | metadata
    if set(actual_files) != expected:
        fail(
            "native_admission_payload_install_manifest_tree_mismatch:"
            f"{context}:missing={sorted(expected - set(actual_files))}:"
            f"unexpected={sorted(set(actual_files) - expected)}"
        )
    sums = _parse_payload_sha256sums(sums_path, context)
    if sums != {rel: digest for rel, (_size, digest) in declared.items()}:
        fail(f"native_admission_payload_sha256sums_manifest_mismatch:{context}")
    for rel, (expected_size, expected_digest) in declared.items():
        path = actual_files[rel]
        if path.stat().st_size != expected_size or sha256_file(path) != expected_digest:
            fail(f"native_admission_payload_install_manifest_binding_mismatch:{context}:{rel}")


def _package_control_text(path: Path, context: str) -> str:
    size = path.stat().st_size
    if size > MAX_PACKAGE_CONTROL_TEXT_BYTES:
        fail(f"native_admission_package_control_too_large:{context}:{path.name}")
    try:
        data = path.read_bytes()
    except OSError as exc:
        fail(f"native_admission_package_control_unreadable:{context}:{path.name}:{exc}")
    if b"\0" in data:
        fail(f"native_admission_package_control_binary:{context}:{path.name}")
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        fail(f"native_admission_package_control_encoding:{context}:{path.name}")


def reject_package_control_text(text: str, relative: str, context: str) -> None:
    """Reject unadmitted identities and payload-construction mechanisms."""

    identity = PACKAGE_CONTROL_IDENTITY.search(text)
    if identity is not None:
        fail(
            "native_admission_package_control_client_forbidden:"
            f"{context}:{relative}:{identity.group(0)}"
        )
    mechanism = PACKAGE_CONTROL_EXECUTION.search(text)
    if mechanism is not None:
        fail(
            "native_admission_package_control_execution_forbidden:"
            f"{context}:{relative}:{mechanism.group(0).strip()}"
        )


def scan_package_control_tree(
    root: Path,
    context: str,
    *,
    allowed_binary_relatives: frozenset[str] = frozenset(),
) -> None:
    """Read every package-control/recipe member and reject hidden payloads.

    ``allowed_binary_relatives`` is deliberately narrow: it is used only for
    the one nested AUR source archive, which is separately unpacked and
    recursively checked.  Every other package-control member must be UTF-8
    text and is checked for client identity and payload-construction commands.
    """

    if not root.is_dir() or root.is_symlink():
        fail(f"native_admission_package_control_root_invalid:{context}")
    files: set[str] = set()
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root).as_posix()
        safe_relative_path(relative, f"package_control_path:{context}")
        if path.is_symlink():
            fail(f"native_admission_package_control_symlink:{context}:{relative}")
        if path.is_dir():
            continue
        if not path.is_file():
            fail(f"native_admission_package_control_special:{context}:{relative}")
        files.add(relative)
        if relative in allowed_binary_relatives:
            continue
        text = _package_control_text(path, context)
        reject_package_control_text(text, relative, context)
    unexpected = allowed_binary_relatives - files
    if unexpected:
        fail(
            "native_admission_package_control_binary_missing:"
            f"{context}:{','.join(sorted(unexpected))}"
        )


def inventory_from_tree(root: Path) -> dict[str, Any]:
    scan_native_only_tree(root, "inventory")
    entries: list[dict[str, Any]] = []
    for path in sorted(item for item in root.rglob("*") if item.is_file()):
        relative = path.relative_to(root).as_posix()
        entries.append(
            {
                "path": relative,
                "bytes": path.stat().st_size,
                "sha256": sha256_file(path),
            }
        )
    if not entries:
        fail("native_admission_inventory_empty")
    rows_digest = canonical_json_sha256(entries)
    return {
        "schema_id": INVENTORY_SCHEMA,
        "recursive": True,
        "policy": NATIVE_POLICY,
        "entry_count": len(entries),
        "entries_sha256": rows_digest,
        "entries": entries,
    }


def validate_inventory(value: object, expected_profile_digest: str, platform: str, context: str) -> None:
    if not isinstance(value, dict):
        fail(f"native_admission_inventory_missing:{context}")
    expected_keys = {
        "schema_id",
        "recursive",
        "policy",
        "entry_count",
        "entries_sha256",
        "entries",
    }
    if set(value) != expected_keys:
        fail(f"native_admission_inventory_fields_invalid:{context}")
    if value.get("schema_id") != INVENTORY_SCHEMA or value.get("recursive") is not True or value.get("policy") != NATIVE_POLICY:
        fail(f"native_admission_inventory_policy_invalid:{context}")
    entries = value.get("entries")
    if not isinstance(entries, list) or not entries or value.get("entry_count") != len(entries):
        fail(f"native_admission_inventory_count_invalid:{context}")
    seen: set[str] = set()
    profile_seen = False
    paths: set[str] = set()
    for row in entries:
        if not isinstance(row, dict) or set(row) != {"path", "bytes", "sha256"}:
            fail(f"native_admission_inventory_row_invalid:{context}")
        rel = row.get("path")
        if not isinstance(rel, str):
            fail(f"native_admission_inventory_path_invalid:{context}")
        safe_relative_path(rel, f"inventory_path:{context}")
        if rel in seen:
            fail(f"native_admission_inventory_path_duplicate:{context}:{rel}")
        seen.add(rel)
        paths.add(rel)
        if not isinstance(row.get("bytes"), int) or row["bytes"] < 0:
            fail(f"native_admission_inventory_size_invalid:{context}:{rel}")
        digest = require_sha256(row.get("sha256"), f"inventory:{context}:{rel}")
        if rel == NATIVE_PROFILE_ARCHIVE_PATH.as_posix():
            if digest != expected_profile_digest:
                fail(f"native_admission_inventory_profile_digest_mismatch:{context}")
            profile_seen = True
    if value.get("entries_sha256") != canonical_json_sha256(entries):
        fail(f"native_admission_inventory_digest_mismatch:{context}")
    if not profile_seen:
        fail(f"native_admission_inventory_profile_missing:{context}")
    suffix = ".exe" if platform == "windows" else ""
    required = {
        f"opt/ScratchBird/bin/{name}{suffix}" for name in REQUIRED_NATIVE_COMPONENTS
    }
    required |= {f"etc/scratchbird/{name}" for name in REQUIRED_NATIVE_CONFIGS}
    missing = sorted(required - paths)
    if missing:
        fail(f"native_admission_inventory_native_payload_missing:{context}:{','.join(missing)}")


def _verify_native_payload_tree(
    root: Path,
    expected_profile_digest: str,
    platform: str,
    architecture: str | None,
    context: str,
    *,
    config_root: Path | None = None,
    payload_layout: str = "portable",
) -> dict[str, Any]:
    validate_payload_layout(root, payload_layout, context)
    verify_declared_installed_payload(root, platform, context)
    profile = root / NATIVE_PROFILE_ARCHIVE_PATH
    if not profile.is_file() or profile.is_symlink():
        fail(f"native_admission_profile_missing:{context}")
    if sha256_file(profile) != expected_profile_digest:
        fail(f"native_server_admission_profile_digest_mismatch:{context}")
    if not NATIVE_PAYLOAD_VERIFIER.is_file():
        fail(f"native_admission_payload_verifier_missing:{NATIVE_PAYLOAD_VERIFIER}")
    command = [sys.executable, str(NATIVE_PAYLOAD_VERIFIER), str(root)]
    if architecture is not None:
        command.extend(("--expected-architecture", architecture))
    if config_root is not None:
        if not config_root.is_dir() or config_root.is_symlink():
            fail(f"native_admission_config_root_missing:{context}:{config_root}")
        command.extend(("--config-root", str(config_root)))
    result = subprocess.run(
        command,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode != 0:
        fail(
            f"native_admission_payload_verification_failed:{context}:"
            f"{result.stdout[-4000:].strip()}"
        )
    inventory = inventory_from_tree(root)
    validate_inventory(inventory, expected_profile_digest, platform, context)
    return inventory


def verify_portable_native_payload(
    archive_path: Path,
    expected_profile_digest: str,
    platform: str,
    architecture: str | None,
) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="scratchbird-native-admission-portable-") as temp_name:
        root = Path(temp_name) / "payload"
        root.mkdir()
        if archive_path.name.endswith(".tar.gz"):
            extract_tar_gz(archive_path, root)
        elif archive_path.suffix.casefold() == ".zip":
            extract_zip(archive_path, root)
        else:
            fail(f"native_admission_portable_format_unsupported:{archive_path.name}")
        return _verify_native_payload_tree(
            root,
            expected_profile_digest,
            platform,
            architecture,
            archive_path.name,
            payload_layout="portable",
        )


def _copy_exact_member(handle: Any, size: int, target: Path, context: str) -> None:
    if size < 0 or size > MAX_TOTAL_BYTES:
        fail(f"native_admission_package_member_size_invalid:{context}:{size}")
    target.parent.mkdir(parents=True, exist_ok=True)
    remaining = size
    with target.open("wb") as output:
        while remaining:
            chunk = handle.read(min(remaining, 1024 * 1024))
            if not chunk:
                fail(f"native_admission_package_member_truncated:{context}")
            output.write(chunk)
            remaining -= len(chunk)


def extract_deb_payload(package_path: Path, target: Path, control_target: Path) -> None:
    members: dict[str, Path] = {}
    try:
        with package_path.open("rb") as handle:
            if handle.read(8) != b"!<arch>\n":
                fail(f"native_admission_deb_magic_invalid:{package_path.name}")
            while True:
                header = handle.read(60)
                if not header:
                    break
                if len(header) != 60 or header[58:60] != b"`\n":
                    fail(f"native_admission_deb_header_invalid:{package_path.name}")
                raw_name = header[:16].decode("ascii", errors="strict").strip().rstrip("/")
                try:
                    size = int(header[48:58].decode("ascii", errors="strict").strip())
                except ValueError:
                    fail(f"native_admission_deb_size_invalid:{package_path.name}:{raw_name}")
                if raw_name not in {"debian-binary", "control.tar.gz", "data.tar.gz"}:
                    fail(f"native_admission_deb_member_forbidden:{package_path.name}:{raw_name}")
                if raw_name in members:
                    fail(f"native_admission_deb_member_duplicate:{package_path.name}:{raw_name}")
                member_path = target.parent / raw_name
                _copy_exact_member(handle, size, member_path, f"{package_path.name}:{raw_name}")
                members[raw_name] = member_path
                if raw_name == "debian-binary" and member_path.read_bytes() != b"2.0\n":
                    fail(f"native_admission_deb_version_invalid:{package_path.name}")
                if size % 2:
                    handle.seek(1, 1)
    except (OSError, UnicodeDecodeError) as exc:
        fail(f"native_admission_deb_invalid:{package_path.name}:{exc}")
    if set(members) != {"debian-binary", "control.tar.gz", "data.tar.gz"}:
        fail(
            "native_admission_deb_member_set_invalid:"
            f"{package_path.name}:{','.join(sorted(members))}"
        )
    extract_tar_gz(members["control.tar.gz"], control_target)
    extract_tar_gz(members["data.tar.gz"], target)


def validate_deb_control_surface(root: Path, context: str) -> None:
    """Require Debian control data to remain an inspectable closed surface."""

    scan_package_control_tree(root, context)
    actual = {
        path.relative_to(root).as_posix()
        for path in root.rglob("*")
        if path.is_file()
    }
    expected = {"control", "postinst", "prerm", "postrm", "conffiles"}
    if actual != expected:
        fail(
            "native_admission_deb_control_set_invalid:"
            f"{context}:missing={sorted(expected - actual)}:"
            f"unexpected={sorted(actual - expected)}"
        )
    for script_name in ("postinst", "prerm", "postrm"):
        text = _package_control_text(root / script_name, context)
        if "/usr/lib/scratchbird/scratchbird-system-install" not in text and script_name != "postrm":
            fail(f"native_admission_deb_control_lifecycle_missing:{context}:{script_name}")
        # No package script may create or copy a runtime payload.  The one
        # config-restore copy in postrm is constrained to /etc/scratchbird.
        for line in text.splitlines():
            stripped = line.strip()
            if re.match(r"(?:^|\s)(?:install|ln|mv)\s", stripped):
                fail(f"native_admission_deb_control_mutation_forbidden:{context}:{script_name}")
            if re.match(r"cp\s", stripped) and stripped != 'cp -an "$preserve_root"/. /etc/scratchbird/':
                fail(f"native_admission_deb_control_copy_forbidden:{context}:{script_name}")


def verify_deb_native_payload(
    package_path: Path,
    expected_profile_digest: str,
    platform: str,
    architecture: str | None,
) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="scratchbird-native-admission-deb-") as temp_name:
        root = Path(temp_name) / "payload"
        control = Path(temp_name) / "control"
        root.mkdir()
        control.mkdir()
        extract_deb_payload(package_path, root, control)
        validate_deb_control_surface(control, package_path.name)
        return _verify_native_payload_tree(
            root,
            expected_profile_digest,
            platform,
            architecture,
            package_path.name,
            payload_layout="linux_system",
        )


def verify_aur_native_payload(
    package_path: Path,
    expected_profile_digest: str,
    platform: str,
    architecture: str | None,
) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="scratchbird-native-admission-aur-") as temp_name:
        outer = Path(temp_name) / "outer"
        outer.mkdir()
        extract_tar_gz(package_path, outer)
        scan_native_only_tree(outer, f"{package_path.name}:outer")
        source_archives = sorted(
            path for path in outer.rglob("*.tar.gz") if path.is_file() and not path.is_symlink()
        )
        if len(source_archives) != 1:
            fail(f"native_admission_aur_source_archive_cardinality:{package_path.name}:{len(source_archives)}")
        validate_aur_control_surface(
            outer,
            source_archives[0].relative_to(outer).as_posix(),
            package_path.name,
        )
        payload = Path(temp_name) / "payload"
        payload.mkdir()
        extract_tar_gz(source_archives[0], payload)
        # The AUR source archive has one versioned directory at its root.
        roots = [path for path in payload.iterdir() if path.is_dir() and not path.is_symlink()]
        if len(roots) != 1:
            fail(f"native_admission_aur_payload_root_cardinality:{package_path.name}:{len(roots)}")
        return _verify_native_payload_tree(
            roots[0],
            expected_profile_digest,
            platform,
            architecture,
            package_path.name,
            payload_layout="linux_system",
        )


def validate_aur_control_surface(
    outer: Path,
    source_archive_relative: str,
    context: str,
) -> None:
    """Validate the entire AUR recipe/install/source wrapper before payload use."""

    expected_files = {
        "scratchbird/PKGBUILD",
        "scratchbird/scratchbird.install",
        source_archive_relative,
    }
    actual_files = {
        path.relative_to(outer).as_posix()
        for path in outer.rglob("*")
        if path.is_file()
    }
    if actual_files != expected_files:
        fail(
            "native_admission_aur_control_set_invalid:"
            f"{context}:missing={sorted(expected_files - actual_files)}:"
            f"unexpected={sorted(actual_files - expected_files)}"
        )
    scan_package_control_tree(
        outer,
        context,
        allowed_binary_relatives=frozenset({source_archive_relative}),
    )
    recipe_path = outer / "scratchbird" / "PKGBUILD"
    install_path = outer / "scratchbird" / "scratchbird.install"
    recipe = _package_control_text(recipe_path, context)
    install = _package_control_text(install_path, context)
    source_name = Path(source_archive_relative).name
    source_match = re.search(r"(?m)^source=\('([^']+)'\)$", recipe)
    if source_match is None or source_match.group(1) != source_name:
        fail(f"native_admission_aur_source_declaration_invalid:{context}")
    if re.search(r"(?i)(?:https?|ftp|git\+)://", source_match.group(1)) is not None:
        fail(f"native_admission_aur_remote_source_forbidden:{context}")
    digest_match = re.search(r"(?m)^sha256sums=\('([0-9a-f]{64})'\)$", recipe)
    archive_path = outer / source_archive_relative
    if digest_match is None or digest_match.group(1) != sha256_file(archive_path):
        fail(f"native_admission_aur_source_digest_mismatch:{context}")
    if "install=scratchbird.install" not in recipe:
        fail(f"native_admission_aur_install_hook_missing:{context}")
    if "/usr/lib/scratchbird/scratchbird-system-install" not in install:
        fail(f"native_admission_aur_lifecycle_missing:{context}")
    for text, label in ((recipe, "PKGBUILD"), (install, "scratchbird.install")):
        for line in text.splitlines():
            stripped = line.strip()
            if re.match(r"(?:^|\s)(?:install|ln|mv)\s", stripped):
                fail(f"native_admission_aur_control_mutation_forbidden:{context}:{label}")
            if re.match(r"cp\s", stripped):
                allowed_copy = re.fullmatch(
                    r'cp -a "\$srcdir"/scratchbird-[^/]+/(?:opt|etc|usr) "\$pkgdir"/',
                    stripped,
                )
                if allowed_copy is None:
                    fail(f"native_admission_aur_control_copy_forbidden:{context}:{label}")
