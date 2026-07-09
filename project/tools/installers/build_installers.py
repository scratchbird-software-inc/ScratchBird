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
    "filesystem_layout": {
        "runtime": "/opt/ScratchBird",
        "configuration": "/etc/scratchbird",
        "launchd": "/Library/LaunchDaemons",
        "logs": "/opt/ScratchBird/var/log",
        "runtime_state": "/opt/ScratchBird/var/run",
    },
}
MACOS_LAUNCHD_SERVICES = (
    ("com.scratchbird.sbsrv", "SBsrv", "SBsrv.conf"),
    ("com.scratchbird.sbgate", "SBgate", "SBgate.conf"),
    ("com.scratchbird.sbmgr", "SBmgr", "SBmgr.conf"),
    ("com.scratchbird.sbparser", "SBParser", "SBParser.conf"),
)
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
    return path.name in {"SHA256SUMS", "PKGBUILD", "control", "postinst", "prerm"}


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


def require_staged_output(artifact_root: Path, platform: str) -> None:
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


def write_install_metadata(root: Path, platform: str, version: str, build_id: str | None) -> None:
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
            "configuration": "/etc/scratchbird",
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


def write_macos_launchd_payload(root: Path) -> None:
    launchd_root = root / "Library" / "LaunchDaemons"
    launchd_root.mkdir(parents=True, exist_ok=True)
    (root / "opt" / "ScratchBird" / "var" / "log").mkdir(parents=True, exist_ok=True)
    (root / "opt" / "ScratchBird" / "var" / "run").mkdir(parents=True, exist_ok=True)
    manifest_rows = []
    for label, binary, config in MACOS_LAUNCHD_SERVICES:
        plist_path = launchd_root / f"{label}.plist"
        payload = {
            "Label": label,
            "ProgramArguments": [
                f"/opt/ScratchBird/bin/{binary}",
                "--config",
                f"/etc/scratchbird/{config}",
            ],
            "RunAtLoad": False,
            "KeepAlive": False,
            "StandardOutPath": f"/opt/ScratchBird/var/log/{label}.out.log",
            "StandardErrorPath": f"/opt/ScratchBird/var/log/{label}.err.log",
            "WorkingDirectory": "/opt/ScratchBird",
        }
        with plist_path.open("wb") as handle:
            plistlib.dump(payload, handle, sort_keys=True)
        plist_path.chmod(0o644)
        manifest_rows.append({"label": label, "binary": binary, "plist": f"/Library/LaunchDaemons/{plist_path.name}"})
    release_dir = root / "opt" / "ScratchBird" / "share" / "scratchbird" / "release"
    release_dir.mkdir(parents=True, exist_ok=True)
    (release_dir / "MACOS_LAUNCHD_MANIFEST.json").write_text(
        json.dumps(
            {
                "schema_id": "scratchbird.macos_launchd_manifest.v1",
                "services": manifest_rows,
                "default_service_state": "installed_disabled_until_admitted_by_policy",
            },
            indent=2,
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )


def stage_install_tree(artifact_root: Path, payload_root: Path, platform: str, version: str, build_id: str | None) -> None:
    if payload_root.exists():
        shutil.rmtree(payload_root)
    (payload_root / "opt" / "ScratchBird").mkdir(parents=True)
    copytree_contents(artifact_root / "bin", payload_root / "opt" / "ScratchBird" / "bin")
    copytree_contents(artifact_root / "lib", payload_root / "opt" / "ScratchBird" / "lib")
    copytree_contents(artifact_root / "share", payload_root / "opt" / "ScratchBird" / "share")
    copytree_contents(artifact_root / "etc", payload_root / "etc")
    for file_name in ("STANDALONE_OUTPUT_MANIFEST.json", "PUBLIC_RELEASE_ARTIFACT_MANIFEST.json"):
        source = artifact_root / file_name
        if source.is_file():
            target = payload_root / "opt" / "ScratchBird" / "share" / "scratchbird" / "release" / file_name
            target.parent.mkdir(parents=True, exist_ok=True)
            sanitize_release_manifest(source, target, platform)
    if platform == "macos":
        write_macos_support_matrix(payload_root)
        write_macos_launchd_payload(payload_root)
    write_install_metadata(payload_root, platform, version, build_id)
    scan_private_text(payload_root)


def make_tarball(payload_root: Path, output_root: Path, version: str, platform: str) -> Path:
    output = output_root / f"scratchbird-{platform}-{version}.tar.gz"
    with tarfile.open(output, "w:gz", format=tarfile.PAX_FORMAT) as archive:
        for path in sorted(payload_root.rglob("*")):
            archive.add(path, arcname=path.relative_to(payload_root).as_posix(), recursive=False)
    return output


def tar_bytes_from_dir(root: Path, mode: str = "w:gz") -> bytes:
    temp = tempfile.NamedTemporaryFile(delete=False)
    temp.close()
    temp_path = Path(temp.name)
    try:
        with tarfile.open(temp_path, mode, format=tarfile.PAX_FORMAT) as archive:
            for path in sorted(root.rglob("*")):
                archive.add(path, arcname=path.relative_to(root).as_posix(), recursive=False)
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
                f"Installed-Size: {max(installed_size, 1)}",
                "Description: ScratchBird Convergent Data Engine pre-release build",
                "",
            ]
        ),
        encoding="utf-8",
    )
    for script_name, text in {
        "postinst": "#!/bin/sh\nset -e\nexit 0\n",
        "prerm": "#!/bin/sh\nset -e\nexit 0\n",
    }.items():
        path = control_root / script_name
        path.write_text(text, encoding="utf-8")
        path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    deb = output_root / f"scratchbird_{sanitize_version(version).replace('-', '+')}_amd64.deb"
    control_tar = tar_bytes_from_dir(control_root)
    data_tar = tar_bytes_from_dir(payload_root)
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
        archive.add(source_root, arcname=f"scratchbird-{rpm_ver}", recursive=True)
    shutil.rmtree(source_root)

    spec = topdir / "SPECS" / "scratchbird.spec"
    spec.write_text(
        f"""Name: scratchbird
Version: {rpm_ver}
Release: {rpm_rel}%{{?dist}}
Summary: ScratchBird Convergent Data Engine pre-release build
License: MPL-2.0
URL: https://scratchbird.com
Source0: scratchbird-{rpm_ver}.tar.gz

%description
ScratchBird Convergent Data Engine pre-release build.

%prep
%setup -q

%build

%install
rm -rf %{{buildroot}}
mkdir -p %{{buildroot}}
cp -a opt %{{buildroot}}/
cp -a etc %{{buildroot}}/

%files
/opt/ScratchBird
/etc/scratchbird
""",
        encoding="utf-8",
    )
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
    return copied or [source_tar, spec]


def make_aur(payload_root: Path, output_root: Path, version: str) -> Path:
    aur_root = output_root / "aur" / "scratchbird"
    if aur_root.exists():
        shutil.rmtree(aur_root)
    aur_root.mkdir(parents=True)
    source_name = f"scratchbird-{sanitize_version(version)}.tar.gz"
    source_path = aur_root / source_name
    with tarfile.open(source_path, "w:gz", format=tarfile.PAX_FORMAT) as archive:
        archive.add(payload_root, arcname=f"scratchbird-{sanitize_version(version)}", recursive=True)
    digest = sha256_file(source_path)
    (aur_root / "PKGBUILD").write_text(
        f"""pkgname=scratchbird
pkgver={sanitize_version(version).replace('-', '_')}
pkgrel=1
pkgdesc='ScratchBird Convergent Data Engine pre-release build'
arch=('x86_64')
url='https://scratchbird.com'
license=('MPL-2.0')
source=('{source_name}')
sha256sums=('{digest}')

package() {{
  cp -a "$srcdir"/scratchbird-{sanitize_version(version)}/opt "$pkgdir"/
  cp -a "$srcdir"/scratchbird-{sanitize_version(version)}/etc "$pkgdir"/
}}
""",
        encoding="utf-8",
    )
    bundle = output_root / f"scratchbird-aur-{sanitize_version(version)}.tar.gz"
    with tarfile.open(bundle, "w:gz", format=tarfile.PAX_FORMAT) as archive:
        archive.add(aur_root, arcname="scratchbird", recursive=True)
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
            "@rpath_or_system_paths_only",
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


def make_macos_pkg(payload_root: Path, output_root: Path, version: str, signing_state: dict[str, Any]) -> Path:
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


def make_wix_msi(payload_root: Path, output_root: Path, version: str, require_msi: bool) -> list[Path]:
    wix_bin = shutil.which("wix")
    wxs = output_root / "scratchbird.wxs"
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
        return [wxs]
    msi = output_root / f"scratchbird-windows-{sanitize_version(version)}.msi"
    run([wix_bin, "build", str(wxs), "-o", str(msi)], cwd=output_root)
    return [msi, wxs]


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
    if platform == "macos":
        manifest["macos"] = {
            "support_matrix": MACOS_SUPPORT_MATRIX,
            "signing_state_file": "MACOS_SIGNING_STATE.json",
            "dynamic_library_audit_file": "MACOS_DYNAMIC_LIBRARY_AUDIT.json",
            "launchd_manifest_file": "opt/ScratchBird/share/scratchbird/release/MACOS_LAUNCHD_MANIFEST.json",
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
    args = parser.parse_args()

    artifact_root = args.artifact_root.resolve()
    output_root = args.output_root.resolve()
    version = sanitize_version(args.version)
    require_staged_output(artifact_root, args.platform)
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
            built.append(make_tarball(payload_root, output_root, version, "linux"))
            built.append(make_deb(payload_root, output_root, version))
            built.extend(make_rpm(payload_root, output_root, version, args.require_rpm))
            built.append(make_aur(payload_root, output_root, version))
        elif args.platform == "windows":
            built.append(make_zip(payload_root, output_root, version))
            built.extend(make_wix_msi(payload_root, output_root, version, args.require_msi))
        else:
            signing_state = maybe_sign_macos_payload(payload_root)
            write_macos_dynamic_library_audit(payload_root, output_root)
            built.append(make_tarball(payload_root, output_root, version, "macos"))
            built.append(make_macos_pkg(payload_root, output_root, version, signing_state))
            write_macos_signing_state(output_root, signing_state)
        manifest = write_artifact_manifest(output_root, args.platform, version, args.build_id)
    print(f"build_installers=passed:{manifest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
