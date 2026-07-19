#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Create the flat, native-only ScratchBird rolling-nightly release bundle."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
from typing import Any
import zipfile

TOOL_ROOT = Path(__file__).resolve().parent
if str(TOOL_ROOT) not in sys.path:
    sys.path.insert(0, str(TOOL_ROOT))

from nightly_release_contract import RELEASE_CONTRACTS, ReleaseContract, get_release_contract


INSTALLER_MANIFEST = "INSTALLER_ARTIFACT_MANIFEST.json"
UNIVERSAL_MANIFEST = "MACOS_UNIVERSAL_ARTIFACT_MANIFEST.json"
SCHEMA_ID = "scratchbird.native_nightly_release.v1"
PUBLIC_ASSET_POLICY = "fully_verified_native_portable_and_system_installer_artifacts"
NATIVE_COMPONENTS = ("SBmgr", "SBgate", "SBParser", "SBsrv")
NATIVE_EXECUTABLES = (
    "SBsrv",
    "SBgate",
    "SBmgr",
    "SBParser",
    "SBsql",
    "SBadm",
    "SBbak",
    "SBsec",
    "SBdoc",
    "SBcop",
)
MACOS_NATIVE_EXECUTABLES = NATIVE_EXECUTABLES + ("SBlaunch",)

ARTIFACT_ROOTS = {
    "linux": "scratchbird-linux-installers",
    "windows": "scratchbird-windows-installers",
    "macos-x86_64": "scratchbird-macos-x86_64-installers",
    "macos-arm64": "scratchbird-macos-arm64-installers",
    "macos-universal": "scratchbird-macos-universal-installers",
}

# Every selected file is first integrity-bound to the platform installer
# manifest from this exact reusable-workflow run.  Portable archives are also
# extracted and validated here.  Native system installers are validated by
# their platform job (installer verifier plus the applicable install-smoke or
# package-recipe check) before the reusable workflow can succeed and make the
# artifact available to this job.
PACKAGE_RULES = (
    (
        "linux", "linux", "x86_64", "tar.gz", "scratchbird-linux-*.tar.gz",
        "scratchbird-nightly-linux-x86_64.tar.gz", "portable_archive",
        "exact_native_payload_extraction",
    ),
    (
        "linux", "linux", "x86_64", "deb", "scratchbird_*.deb",
        "scratchbird-nightly-linux-x86_64.deb", "system_installer",
        "installer_manifest_and_privileged_deb_smoke",
    ),
    (
        "linux", "linux", "x86_64", "rpm", "scratchbird-*.x86_64.rpm",
        "scratchbird-nightly-linux-x86_64.rpm", "system_installer",
        "installer_manifest_and_rpm_recipe_verification",
    ),
    (
        "linux", "linux", "x86_64", "aur.tar.gz", "scratchbird-aur-*.tar.gz",
        "scratchbird-nightly-linux-x86_64-aur.tar.gz", "system_installer",
        "installer_manifest_and_aur_recipe_verification",
    ),
    (
        "windows", "windows", "x86_64", "zip", "scratchbird-windows-*.zip",
        "scratchbird-nightly-windows-x86_64.zip", "portable_archive",
        "exact_native_payload_extraction",
    ),
    (
        "windows", "windows", "x86_64", "msi", "scratchbird-windows-*.msi",
        "scratchbird-nightly-windows-x86_64.msi", "system_installer",
        "installer_manifest_and_msi_smoke",
    ),
    (
        "macos-x86_64", "macos", "x86_64", "tar.gz", "scratchbird-macos-*.tar.gz",
        "scratchbird-nightly-macos-x86_64.tar.gz", "portable_archive",
        "exact_native_payload_extraction",
    ),
    (
        "macos-x86_64", "macos", "x86_64", "pkg", "scratchbird-macos-*.pkg",
        "scratchbird-nightly-macos-x86_64.pkg", "system_installer",
        "installer_manifest_and_pkg_smoke",
    ),
    (
        "macos-arm64", "macos", "arm64", "tar.gz", "scratchbird-macos-*.tar.gz",
        "scratchbird-nightly-macos-arm64.tar.gz", "portable_archive",
        "exact_native_payload_extraction",
    ),
    (
        "macos-arm64", "macos", "arm64", "pkg", "scratchbird-macos-*.pkg",
        "scratchbird-nightly-macos-arm64.pkg", "system_installer",
        "installer_manifest_and_pkg_smoke",
    ),
)

FORBIDDEN_TEXT = (
    "ScratchBird" + "-Private",
    "/home/",
    "\\home\\",
    "/local" + "_work",
    "\\local" + "_work",
    "docs/workplans",
    "docs/specifications",
    "packaging/",
)


class BundleError(RuntimeError):
    """A fail-closed nightly bundle validation error."""


def fail(message: str) -> None:
    raise BundleError(message)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sanitize_version(value: str) -> str:
    cleaned = re.sub(r"[^0-9A-Za-z.+~_-]", ".", value.strip()).strip(".-_")
    return cleaned or "0.0.0-nightly"


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        fail(f"json_invalid:{path.name}:{exc}")
    if not isinstance(value, dict):
        fail(f"json_root_not_object:{path.name}")
    return value


def safe_relative(value: str, context: str) -> Path:
    path = Path(value)
    if not value or path.is_absolute() or "\\" in value or any(part in {"", ".", ".."} for part in path.parts):
        fail(f"unsafe_relative_path:{context}:{value}")
    return path


def require_single(root: Path, filename: str) -> Path:
    matches = sorted(path for path in root.rglob(filename) if path.is_file() and not path.is_symlink())
    if len(matches) != 1:
        fail(f"file_cardinality:{root.name}:{filename}:expected=1:actual={len(matches)}")
    return matches[0]


def ensure_safe_tree(root: Path) -> None:
    for path in root.rglob("*"):
        if path.is_symlink():
            fail(f"symlink_forbidden:{path.relative_to(root).as_posix()}")


def parse_sha256sums(path: Path) -> dict[str, str]:
    rows: dict[str, str] = {}
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        fail(f"sha256sums_unreadable:{path.name}:{exc}")
    for line in lines:
        match = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
        if match is None:
            fail(f"sha256sums_row_invalid:{path.name}:{line}")
        digest, rel = match.groups()
        safe_relative(rel, "sha256sums")
        if rel in rows:
            fail(f"sha256sums_duplicate:{rel}")
        rows[rel] = digest
    if not rows:
        fail(f"sha256sums_empty:{path.name}")
    return rows


def verify_installer_root(
    root: Path,
    expected_platform: str,
    expected_version: str,
    expected_build_id: str,
) -> tuple[dict[str, Any], dict[str, Path]]:
    manifest_path = require_single(root, INSTALLER_MANIFEST)
    artifact_root = manifest_path.parent
    data = load_json(manifest_path)
    if data.get("schema_id") != "scratchbird.installer_artifact_manifest.v1":
        fail(f"installer_manifest_schema:{root.name}")
    if data.get("platform") != expected_platform:
        fail(f"installer_manifest_platform:{root.name}:{data.get('platform')}")
    manifest_version = data.get("version")
    if not isinstance(manifest_version, str) or not manifest_version or sanitize_version(manifest_version) != expected_version:
        fail(f"installer_manifest_version:{root.name}:{data.get('version')}")
    if data.get("build_id") != expected_build_id:
        fail(f"installer_manifest_build_id:{root.name}:{data.get('build_id')}:{expected_build_id}")
    rows = data.get("artifacts")
    if not isinstance(rows, list) or not rows:
        fail(f"installer_manifest_artifacts_missing:{root.name}")

    verified: dict[str, Path] = {}
    expected_sums: dict[str, str] = {}
    for row in rows:
        if not isinstance(row, dict):
            fail(f"installer_manifest_row_not_object:{root.name}")
        rel_value = row.get("path")
        if not isinstance(rel_value, str):
            fail(f"installer_manifest_path_invalid:{root.name}")
        rel = safe_relative(rel_value, "installer_manifest")
        rel_name = rel.as_posix()
        if rel_name in verified:
            fail(f"installer_manifest_path_duplicate:{root.name}:{rel_name}")
        source = artifact_root / rel
        if not source.is_file() or source.is_symlink():
            fail(f"installer_manifest_file_missing:{root.name}:{rel_name}")
        size = row.get("bytes")
        digest = row.get("sha256")
        if not isinstance(size, int) or size < 0 or source.stat().st_size != size:
            fail(f"installer_manifest_size_mismatch:{root.name}:{rel_name}")
        actual = sha256_file(source)
        if not isinstance(digest, str) or digest != actual:
            fail(f"installer_manifest_sha256_mismatch:{root.name}:{rel_name}")
        verified[rel_name] = source
        expected_sums[rel_name] = actual

    sums = parse_sha256sums(artifact_root / "SHA256SUMS")
    if sums != expected_sums:
        fail(f"installer_sha256sums_manifest_mismatch:{root.name}")
    if expected_platform == "macos":
        for sidecar in ("MACOS_DYNAMIC_LIBRARY_AUDIT.json", "MACOS_SIGNING_STATE.json"):
            if sidecar not in verified:
                fail(f"macos_sidecar_missing:{root.name}:{sidecar}")
        audit = load_json(verified["MACOS_DYNAMIC_LIBRARY_AUDIT.json"])
        if not isinstance(audit.get("rows"), list) or not audit["rows"]:
            fail(f"macos_dynamic_library_audit_empty:{root.name}")
        signing = load_json(verified["MACOS_SIGNING_STATE.json"])
        if signing.get("status") not in {"qa_unsigned_not_for_public_signed_release", "payload_signed"}:
            fail(f"macos_signing_state_invalid:{root.name}:{signing.get('status')}")
    return data, verified


def select_package(
    root_name: str,
    verified: dict[str, Path],
    pattern: str,
    required: bool,
) -> Path | None:
    matches = sorted(
        path
        for rel, path in verified.items()
        if "/" not in rel and Path(rel).match(pattern)
    )
    if len(matches) > 1 or (required and len(matches) != 1):
        fail(f"package_cardinality:{root_name}:{pattern}:expected={'1' if required else '0_or_1'}:actual={len(matches)}")
    return matches[0] if matches else None


def validate_archive_name(value: str, context: str) -> str:
    normalized = value.rstrip("/")
    if not normalized:
        return normalized
    safe_relative(normalized, context)
    return normalized


def write_extracted_file(root: Path, name: str, source: Any, size: int, context: str) -> None:
    if size < 0 or size > 16 * 1024 * 1024 * 1024:
        fail(f"archive_member_size_invalid:{context}:{name}:{size}")
    target = root / name
    target.parent.mkdir(parents=True, exist_ok=True)
    with target.open("wb") as handle:
        shutil.copyfileobj(source, handle, length=1024 * 1024)
    if target.stat().st_size != size:
        fail(f"archive_member_size_mismatch:{context}:{name}")


def safely_extract_archive(path: Path, target: Path) -> None:
    names: set[str] = set()
    member_count = 0
    total_size = 0
    if path.name.endswith(".tar.gz"):
        try:
            with tarfile.open(path, "r:gz") as archive:
                for member in archive:
                    member_count += 1
                    if member_count > 100_000:
                        fail(f"archive_member_limit:{path.name}")
                    name = validate_archive_name(member.name, f"archive:{path.name}")
                    if not name:
                        continue
                    if name in names:
                        fail(f"archive_member_duplicate:{path.name}:{name}")
                    names.add(name)
                    if member.issym() or member.islnk() or member.isdev():
                        fail(f"archive_link_or_device_forbidden:{path.name}:{name}")
                    if member.isdir():
                        (target / name).mkdir(parents=True, exist_ok=True)
                        continue
                    if not member.isfile():
                        fail(f"archive_member_type_forbidden:{path.name}:{name}")
                    total_size += member.size
                    if total_size > 16 * 1024 * 1024 * 1024:
                        fail(f"archive_uncompressed_size_limit:{path.name}")
                    source = archive.extractfile(member)
                    if source is None:
                        fail(f"archive_member_unreadable:{path.name}:{name}")
                    write_extracted_file(target, name, source, member.size, path.name)
        except (OSError, tarfile.TarError) as exc:
            fail(f"native_tar_invalid:{path.name}:{exc}")
        return
    if path.name.endswith(".zip"):
        try:
            with zipfile.ZipFile(path) as archive:
                for member in archive.infolist():
                    member_count += 1
                    if member_count > 100_000:
                        fail(f"archive_member_limit:{path.name}")
                    name = validate_archive_name(member.filename, f"archive:{path.name}")
                    if not name:
                        continue
                    if name in names:
                        fail(f"archive_member_duplicate:{path.name}:{name}")
                    names.add(name)
                    mode = (member.external_attr >> 16) & 0o170000
                    if mode == 0o120000:
                        fail(f"archive_symlink_forbidden:{path.name}:{name}")
                    if member.is_dir():
                        (target / name).mkdir(parents=True, exist_ok=True)
                        continue
                    total_size += member.file_size
                    if total_size > 16 * 1024 * 1024 * 1024:
                        fail(f"archive_uncompressed_size_limit:{path.name}")
                    with archive.open(member) as source:
                        write_extracted_file(target, name, source, member.file_size, path.name)
        except (OSError, RuntimeError, zipfile.BadZipFile) as exc:
            fail(f"native_zip_invalid:{path.name}:{exc}")
        return
    fail(f"native_payload_archive_format_unsupported:{path.name}")


def macho_architectures(path: Path) -> set[str]:
    with path.open("rb") as handle:
        data = handle.read(4096)
    if len(data) < 8:
        fail(f"macho_header_too_short:{path.name}")
    cpu_names = {0x01000007: "x86_64", 0x0100000C: "arm64"}
    thin_magics = {
        b"\xcf\xfa\xed\xfe": "<",
        b"\xfe\xed\xfa\xcf": ">",
    }
    if data[:4] in thin_magics:
        cpu = struct.unpack_from(f"{thin_magics[data[:4]]}I", data, 4)[0]
        if cpu not in cpu_names:
            fail(f"macho_cpu_unsupported:{path.name}:{cpu:#x}")
        return {cpu_names[cpu]}
    fat_magics = {
        b"\xca\xfe\xba\xbe": (">", 20),
        b"\xbe\xba\xfe\xca": ("<", 20),
        b"\xca\xfe\xba\xbf": (">", 32),
        b"\xbf\xba\xfe\xca": ("<", 32),
    }
    if data[:4] not in fat_magics:
        fail(f"macho_magic_invalid:{path.name}:{data[:4].hex()}")
    endian, stride = fat_magics[data[:4]]
    count = struct.unpack_from(f"{endian}I", data, 4)[0]
    if count < 1 or count > 16 or len(data) < 8 + count * stride:
        fail(f"macho_fat_header_invalid:{path.name}:{count}")
    result: set[str] = set()
    for index in range(count):
        cpu = struct.unpack_from(f"{endian}I", data, 8 + index * stride)[0]
        if cpu not in cpu_names:
            fail(f"macho_cpu_unsupported:{path.name}:{cpu:#x}")
        result.add(cpu_names[cpu])
    return result


def verify_macos_architecture(payload_root: Path, expected_architectures: set[str], context: str) -> None:
    for executable in MACOS_NATIVE_EXECUTABLES:
        path = payload_root / "opt" / "ScratchBird" / "bin" / executable
        if not path.is_file():
            fail(f"macos_native_executable_missing:{context}:{executable}")
        actual = macho_architectures(path)
        if actual != expected_architectures:
            fail(
                f"macos_architecture_mismatch:{context}:{executable}:"
                f"expected={sorted(expected_architectures)}:actual={sorted(actual)}"
            )


def verify_native_payload_archive(path: Path, platform: str, architecture: str) -> None:
    verifier = Path(__file__).resolve().parents[1] / "release" / "verify_native_installed_payload.py"
    if not verifier.is_file():
        fail(f"native_payload_verifier_missing:{verifier.name}")
    with tempfile.TemporaryDirectory(prefix="scratchbird-nightly-native-proof-") as temp_name:
        payload_root = Path(temp_name)
        safely_extract_archive(path, payload_root)
        completed = subprocess.run(
            [
                sys.executable,
                str(verifier),
                str(payload_root),
                "--expected-architecture",
                architecture,
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        if completed.returncode != 0:
            fail(f"native_payload_verification_failed:{path.name}:{completed.stdout.strip()}")
        if platform == "macos":
            expected = {"x86_64", "arm64"} if architecture == "universal" else {architecture}
            verify_macos_architecture(payload_root, expected, path.name)


def verify_universal_root(root: Path, expected_version: str, expected_build_id: str) -> Path:
    manifest_path = require_single(root, UNIVERSAL_MANIFEST)
    data = load_json(manifest_path)
    if data.get("schema_id") != "scratchbird.macos_universal_artifact_manifest.v1":
        fail("macos_universal_manifest_schema")
    universal_version = data.get("version")
    expected_universal_version = sanitize_version(f"{expected_version}-universal")
    if not isinstance(universal_version, str) or sanitize_version(universal_version) != expected_universal_version:
        fail(f"macos_universal_version_mismatch:{universal_version}")
    if data.get("build_id") != expected_build_id:
        fail(f"macos_universal_build_id_mismatch:{data.get('build_id')}:{expected_build_id}")
    artifact = data.get("artifact")
    if not isinstance(artifact, dict):
        fail("macos_universal_artifact_missing")
    rel_value = artifact.get("path")
    if not isinstance(rel_value, str):
        fail("macos_universal_artifact_path_invalid")
    rel = safe_relative(rel_value, "macos_universal_artifact")
    source = manifest_path.parent / rel
    if not source.is_file() or source.is_symlink():
        fail(f"macos_universal_file_missing:{rel.as_posix()}")
    if (
        source.parent != manifest_path.parent
        or not source.name.startswith("scratchbird-macos-universal-")
        or not source.name.endswith(".tar.gz")
    ):
        fail(f"macos_universal_filename_invalid:{rel.as_posix()}")
    if artifact.get("bytes") != source.stat().st_size:
        fail("macos_universal_size_mismatch")
    if artifact.get("sha256") != sha256_file(source):
        fail("macos_universal_sha256_mismatch")
    if artifact.get("architectures") != ["x86_64", "arm64"]:
        fail("macos_universal_architectures_mismatch")
    if artifact.get("status") != "qa_universal_after_per_architecture_artifacts_verify":
        fail("macos_universal_status_mismatch")
    return source


def copy_package(
    source: Path,
    output_root: Path,
    canonical_name: str,
    platform: str,
    architecture: str,
    package_format: str,
    verification: str,
) -> dict[str, Any]:
    target = output_root / canonical_name
    if target.exists():
        fail(f"canonical_collision:{canonical_name}")
    shutil.copy2(source, target)
    return {
        "name": canonical_name,
        "platform": platform,
        "architecture": architecture,
        "format": package_format,
        "source_name": source.name,
        "verification": verification,
        "bytes": target.stat().st_size,
        "sha256": sha256_file(target),
    }


def scan_output(root: Path, checksum_name: str) -> None:
    for path in sorted(item for item in root.iterdir() if item.is_file()):
        if path.is_symlink():
            fail(f"output_symlink_forbidden:{path.name}")
        if path.stat().st_size > 2 * 1024 * 1024:
            continue
        if path.suffix.lower() not in {".json", ".txt"} and path.name != checksum_name:
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        for fragment in FORBIDDEN_TEXT:
            if fragment in text:
                fail(f"forbidden_text:{path.name}:{fragment}")


def create_bundle(
    input_root: Path,
    output_root: Path,
    version: str,
    source_revision: str,
    github_run_id: str,
    github_run_attempt: str,
    release_scope: str = "all",
) -> Path:
    if input_root.is_symlink() or output_root.is_symlink():
        fail("input_output_symlink_forbidden")
    input_root = input_root.resolve()
    output_root = output_root.resolve()
    if not input_root.is_dir():
        fail(f"input_root_missing:{input_root}")
    if input_root == output_root or input_root in output_root.parents or output_root in input_root.parents:
        fail("input_output_overlap")
    if not re.fullmatch(r"[0-9a-fA-F]{40,64}", source_revision):
        fail("source_revision_invalid")
    if not github_run_id.isdigit() or not github_run_attempt.isdigit():
        fail("github_run_identity_invalid")
    try:
        contract = get_release_contract(release_scope)
    except ValueError as exc:
        fail(str(exc))
    expected_version = sanitize_version(version)
    ensure_safe_tree(input_root)

    roots: dict[str, Path] = {}
    for key in contract.artifact_roots:
        dirname = ARTIFACT_ROOTS[key]
        path = input_root / dirname
        if not path.is_dir():
            fail(f"artifact_root_missing:{dirname}")
        roots[key] = path
    unexpected = sorted(
        path.name
        for path in input_root.iterdir()
        if path.name not in {ARTIFACT_ROOTS[key] for key in contract.artifact_roots}
    )
    if unexpected:
        fail(f"unexpected_artifact_roots:{','.join(unexpected)}")

    verified: dict[str, dict[str, Path]] = {}
    verify_platforms_all = {
        "linux": ("linux", github_run_id),
        "windows": ("windows", github_run_id),
        "macos-x86_64": ("macos", f"{github_run_id}-x86_64"),
        "macos-arm64": ("macos", f"{github_run_id}-arm64"),
    }
    for key in contract.artifact_roots:
        if key == "macos-universal":
            continue
        platform, expected_build_id = verify_platforms_all[key]
        _, verified[key] = verify_installer_root(
            roots[key], platform, expected_version, expected_build_id
        )

    if output_root.exists():
        shutil.rmtree(output_root)
    output_root.mkdir(parents=True)
    records: list[dict[str, Any]] = []
    selected_rules = tuple(
        rule for rule in PACKAGE_RULES if rule[0] in contract.artifact_roots
    )
    for root_key, platform, arch, package_format, pattern, canonical, verification, verification_record in selected_rules:
        source = select_package(root_key, verified[root_key], pattern, True)
        if verification == "portable_archive":
            verify_native_payload_archive(source, platform, arch)
        elif verification == "system_installer":
            pass
        else:
            fail(f"package_verification_mode_invalid:{canonical}:{verification}")
        records.append(
            copy_package(
                source,
                output_root,
                canonical,
                platform,
                arch,
                package_format,
                verification_record,
            )
        )

    if "macos-universal" in contract.artifact_roots:
        universal_source = verify_universal_root(
            roots["macos-universal"], expected_version, f"{github_run_id}-universal"
        )
        verify_native_payload_archive(universal_source, "macos", "universal")
        records.append(
            copy_package(
                universal_source,
                output_root,
                "scratchbird-nightly-macos-universal.tar.gz",
                "macos",
                "universal",
                "tar.gz",
                "exact_native_payload_extraction",
            )
        )
    records.sort(key=lambda row: row["name"])
    record_names = tuple(record["name"] for record in records)
    if record_names != tuple(sorted(contract.package_names)):
        fail(
            "release_contract_package_inventory_mismatch:"
            f"expected={sorted(contract.package_names)}:actual={list(record_names)}"
        )

    llvm_runtime = {
        "linux": {
            "delivery": "system-package",
            "minimum_major": 23,
            "packages": ["libllvm23", "llvm-libs >= 23"],
            "portable_archive_requires_preinstallation": True,
        },
        "windows": {
            "delivery": "bundled",
            "minimum_major": 22,
            "dll_and_non_system_import_closure_included": True,
        },
        "macos": {
            "delivery": "external-homebrew",
            "minimum_major": 22,
            "formula": "llvm",
            "homebrew_prefix_relative_path": "lib/libLLVM.dylib",
        },
    }
    included_platforms = (
        ("linux", "windows", "macos")
        if contract.scope == "all"
        else (contract.scope,)
    )

    manifest = {
        "schema_id": SCHEMA_ID,
        "generated_at_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
        "channel": "nightly",
        "release_scope": contract.scope,
        "release_tag": contract.tag,
        "included_platforms": list(included_platforms),
        "version": expected_version,
        "source_revision": source_revision.lower(),
        "github_run_id": github_run_id,
        "github_run_attempt": github_run_attempt,
        "distribution_surface": "scratchbird_native_no_emulation",
        "native_parser": "SBSQL",
        "native_components": [
            {"name": "SBmgr", "role": "manager"},
            {"name": "SBgate", "role": "gateway"},
            {"name": "SBParser", "role": "native_SBSQL_parser"},
            {"name": "SBsrv", "role": "database_server"},
        ],
        "emulation_layers_included": False,
        "public_asset_policy": PUBLIC_ASSET_POLICY,
        "internal_only_artifact_classes": ["build_recipes", "install_smoke_proof"],
        "llvm_runtime": {platform: llvm_runtime[platform] for platform in included_platforms},
        "artifacts": records,
    }
    if "macos" in included_platforms:
        manifest["macos_release_policy"] = "qa_unsigned_unnotarized_unless_signing_state_says_payload_signed"
        manifest["macos_signing_state"] = {
            "x86_64": load_json(verified["macos-x86_64"]["MACOS_SIGNING_STATE.json"])["status"],
            "arm64": load_json(verified["macos-arm64"]["MACOS_SIGNING_STATE.json"])["status"],
        }
    manifest_path = output_root / contract.manifest_name
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    checksum_files = sorted(
        path for path in output_root.iterdir() if path.is_file() and path.name != contract.checksum_name
    )
    (output_root / contract.checksum_name).write_text(
        "".join(f"{sha256_file(path)}  {path.name}\n" for path in checksum_files),
        encoding="utf-8",
    )
    if parse_sha256sums(output_root / contract.checksum_name) != {
        path.name: sha256_file(path) for path in checksum_files
    }:
        fail("release_checksum_self_verification_failed")
    scan_output(output_root, contract.checksum_name)
    expected_count = len(contract.package_names) + 2
    actual_count = sum(1 for path in output_root.iterdir() if path.is_file())
    if actual_count != expected_count:
        fail(f"release_asset_count:expected={expected_count}:actual={actual_count}")
    return manifest_path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-root", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--source-revision", required=True)
    parser.add_argument("--github-run-id", required=True)
    parser.add_argument("--github-run-attempt", required=True)
    parser.add_argument("--release-scope", choices=tuple(RELEASE_CONTRACTS), default="all")
    args = parser.parse_args()
    try:
        manifest = create_bundle(
            args.input_root,
            args.output_root,
            args.version,
            args.source_revision,
            args.github_run_id,
            args.github_run_attempt,
            args.release_scope,
        )
    except BundleError as exc:
        print(f"create_nightly_release_bundle=fail:{exc}", file=sys.stderr)
        return 1
    print(f"create_nightly_release_bundle=passed:{manifest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
