#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Create an upload-ready web distribution bundle from installer artifacts."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path, PureWindowsPath
import shutil
import sys
from typing import Any

import installer_native_admission as native_admission


INSTALLER_MANIFEST = "INSTALLER_ARTIFACT_MANIFEST.json"
MACOS_UNIVERSAL_MANIFEST = "MACOS_UNIVERSAL_ARTIFACT_MANIFEST.json"
# The web workflow downloads only named installer artifacts with
# ``merge-multiple: false``.  Keep that namespace exact so a newly uploaded
# artifact cannot become a public package merely because it happens to carry a
# generic installer manifest.
INSTALLER_ARTIFACT_ROOTS = {
    "scratchbird-linux-installers": ("linux", "x86_64"),
    "scratchbird-windows-installers": ("windows", "x86_64"),
    "scratchbird-macos-x86_64-installers": ("macos", "x86_64"),
    "scratchbird-macos-arm64-installers": ("macos", "arm64"),
}
MACOS_UNIVERSAL_ARTIFACT_ROOT = "scratchbird-macos-universal-installers"
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
    print(f"create_web_distribution_bundle=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def native_admission_call(callable_: Any, *args: Any, **kwargs: Any) -> Any:
    """Translate shared admission failures into this command's stable error."""

    try:
        return callable_(*args, **kwargs)
    except native_admission.NativeAdmissionError as exc:
        fail(str(exc))


def reject_cross_platform_path(value: str, context: str) -> None:
    """Reject paths that are absolute on either POSIX or Windows.

    GitHub archives may be created on a platform different from the verifier.
    In particular, ``Path('C:/...')`` is relative on Linux.
    """

    windows = PureWindowsPath(value)
    if Path(value).is_absolute() or windows.is_absolute() or windows.drive:
        fail(f"cross_platform_absolute_path_forbidden:{context}:{value}")


def reject_public_path(value: str, context: str) -> None:
    reject_cross_platform_path(value, context)
    if Path(value).is_absolute():
        fail(f"absolute_path_forbidden:{context}:{value}")
    for fragment in FORBIDDEN_TEXT:
        if fragment in value:
            fail(f"forbidden_path_fragment:{context}:{fragment}:{value}")


def reject_client_payload_identity(value: str, context: str) -> None:
    native_admission_call(
        native_admission.reject_client_payload_identity,
        value,
        context,
    )


def safe_manifest_relative_path(value: str, context: str) -> Path:
    """Return a native-admission-safe relative manifest path."""

    reject_cross_platform_path(value, context)
    return native_admission_call(native_admission.safe_relative_path, value, context)


def safe_component(value: str, context: str) -> str:
    if not value or value in {".", ".."} or "/" in value or "\\" in value:
        fail(f"unsafe_component:{context}:{value}")
    reject_public_path(value, context)
    reject_client_payload_identity(value, context)
    return value


def sanitize_channel(value: str) -> str:
    channel = value.strip().lower().replace("_", "-")
    allowed = {"nightly", "beta", "pre-release", "release-candidate", "qa"}
    if channel not in allowed:
        fail(f"unsupported_channel:{value}")
    return channel


def sanitize_version(value: str) -> str:
    cleaned = "".join(ch if ch.isalnum() or ch in ".+~_-" else "." for ch in value.strip())
    cleaned = cleaned.strip(".-_")
    return cleaned or "0.0.0-nightly"


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        fail(f"json_invalid:{path.name}:{exc}")


def is_text_candidate(path: Path) -> bool:
    if path.stat().st_size > 2 * 1024 * 1024:
        return False
    return path.suffix.lower() in {
        ".json",
        ".md",
        ".txt",
        ".csv",
        ".xml",
        ".wxs",
        ".spec",
        ".service",
        ".plist",
        ".sh",
        ".ps1",
    } or path.name in {"SHA256SUMS", "PKGBUILD"}


def scan_private_text(root: Path) -> None:
    for path in sorted(item for item in root.rglob("*") if item.is_file()):
        rel = path.relative_to(root).as_posix()
        reject_public_path(rel, "output_path")
        reject_client_payload_identity(rel, "output_path")
        if not is_text_candidate(path):
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        for fragment in FORBIDDEN_TEXT:
            if fragment in text:
                fail(f"forbidden_text_fragment:{rel}:{fragment}")


def require_native_server_admission(
    data: dict[str, Any],
    manifest_path: Path,
) -> tuple[dict[str, Any], str]:
    """Validate the manifest through the shared native-admission contract."""

    return native_admission_call(
        native_admission.require_native_server_admission,
        data.get("native_server_admission"),
        str(manifest_path),
    )


def required_portable_archive_suffix(platform: str) -> str:
    if platform in {"linux", "macos"}:
        return ".tar.gz"
    if platform == "windows":
        return ".zip"
    fail(f"portable_platform_unsupported:{platform}")


def require_portable_archive_name(
    archive_path: Path,
    platform: str,
    *,
    universal: bool = False,
) -> None:
    """Allow only the portable asset names published on the web surface."""

    patterns = {
        "linux": "scratchbird-linux-*.tar.gz",
        "windows": "scratchbird-windows-*.zip",
        "macos": (
            "scratchbird-macos-universal-*.tar.gz"
            if universal
            else "scratchbird-macos-*.tar.gz"
        ),
    }
    pattern = patterns.get(platform)
    if pattern is None or not archive_path.match(pattern):
        fail(f"portable_archive_name_invalid:{platform}:{archive_path.name}")


def verify_published_native_archive(
    archive_path: Path,
    expected_digest: str,
    platform: str,
    architecture: str,
    *,
    universal: bool = False,
) -> None:
    """Admit a published portable archive only through the shared verifier.

    This tool intentionally owns no copy of profile parsing or runtime-tree
    policy.  The shared extracted-payload verifier is the one authority, so a
    policy strengthening applies to web exports immediately.
    """

    expected_suffix = required_portable_archive_suffix(platform)
    if not archive_path.name.endswith(expected_suffix):
        fail(f"portable_archive_suffix_invalid:{platform}:{archive_path.name}")
    require_portable_archive_name(archive_path, platform, universal=universal)
    native_admission_call(
        native_admission.verify_portable_native_payload,
        archive_path,
        expected_digest,
        platform,
        architecture,
    )


def copy_file(
    source: Path,
    target: Path,
    record_root: Path,
    copied: dict[str, str],
    records: list[dict[str, Any]],
    attrs: dict[str, Any],
) -> None:
    if not source.is_file():
        fail(f"source_file_missing:{source}")
    rel = target.relative_to(record_root).as_posix()
    reject_public_path(rel, "web_path")
    reject_client_payload_identity(rel, "web_path")
    digest = sha256_file(source)
    existing = copied.get(rel)
    if existing is not None and existing != digest:
        fail(f"web_path_collision:{rel}")
    if existing is not None:
        return
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)
    copied[rel] = digest
    records.append(
        {
            **attrs,
            "path": rel,
            "bytes": target.stat().st_size,
            "sha256": digest,
        }
    )


def require_exact_installer_root(
    manifest_path: Path,
    artifact_paths: set[str],
) -> None:
    """Require the downloaded artifact root to contain only manifest-bound files."""

    root = manifest_path.parent
    expected = {INSTALLER_MANIFEST, "SHA256SUMS"} | artifact_paths
    actual: set[str] = set()
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root).as_posix()
        safe_manifest_relative_path(relative, f"installer_root:{root.name}")
        if path.is_symlink():
            fail(f"web_input_artifact_root_symlink:{root.name}:{relative}")
        if path.is_file():
            actual.add(relative)
        elif not path.is_dir():
            fail(f"web_input_artifact_root_special_entry:{root.name}:{relative}")
    if actual != expected:
        fail(
            f"web_input_artifact_root_content_mismatch:{root.name}:"
            f"missing={sorted(expected - actual)}:unexpected={sorted(actual - expected)}"
        )


def installer_files(
    manifest_path: Path,
    data: dict[str, Any],
    platform: str,
) -> tuple[list[Path], Path]:
    root = manifest_path.parent
    sha_path = root / "SHA256SUMS"
    if not sha_path.is_file() or sha_path.is_symlink():
        fail(f"installer_sha256sums_missing:{manifest_path}")
    files = [manifest_path, sha_path]
    artifacts = data.get("artifacts")
    if not isinstance(artifacts, list):
        fail(f"installer_manifest_artifacts_missing:{manifest_path}")
    if len(artifacts) != 1:
        fail(f"portable_artifact_cardinality:{manifest_path}:{len(artifacts)}")
    artifact_paths: set[str] = set()
    archive: Path | None = None
    for row in artifacts:
        if not isinstance(row, dict):
            fail(f"installer_manifest_row_invalid:{manifest_path}")
        rel = row.get("path")
        if not isinstance(rel, str):
            fail(f"installer_manifest_row_path_invalid:{manifest_path}")
        relative = safe_manifest_relative_path(rel, "installer_manifest_path")
        rel = relative.as_posix()
        if rel in artifact_paths:
            fail(f"installer_manifest_path_duplicate:{manifest_path}:{rel}")
        artifact_paths.add(rel)
        path = root / relative
        if not path.is_file() or path.is_symlink():
            fail(f"installer_manifest_file_missing:{rel}")
        expected = row.get("sha256")
        actual = sha256_file(path)
        if expected != actual:
            fail(f"installer_manifest_sha256_mismatch:{rel}")
        files.append(path)
        archive = path
    require_exact_installer_root(manifest_path, artifact_paths)
    if archive is None:
        fail(f"portable_archive_missing:{manifest_path}")
    return files, archive


def require_exact_universal_root(manifest_path: Path, artifact_relative: str) -> None:
    """Reject unmanifested files beside the macOS universal portable archive."""

    root = manifest_path.parent
    expected = {MACOS_UNIVERSAL_MANIFEST, artifact_relative}
    actual: set[str] = set()
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root).as_posix()
        safe_manifest_relative_path(relative, f"universal_root:{root.name}")
        if path.is_symlink():
            fail(f"web_input_universal_root_symlink:{root.name}:{relative}")
        if path.is_file():
            actual.add(relative)
        elif not path.is_dir():
            fail(f"web_input_universal_root_special_entry:{root.name}:{relative}")
    if actual != expected:
        fail(
            f"web_input_universal_root_content_mismatch:{root.name}:"
            f"missing={sorted(expected - actual)}:unexpected={sorted(actual - expected)}"
        )


def require_expected_installer_root(
    input_root: Path,
    manifest_path: Path,
) -> tuple[str, str]:
    try:
        relative = manifest_path.relative_to(input_root)
    except ValueError:
        fail(f"installer_manifest_outside_input_root:{manifest_path}")
    if len(relative.parts) != 2 or relative.name != INSTALLER_MANIFEST:
        fail(f"installer_manifest_location_invalid:{relative.as_posix()}")
    expected = INSTALLER_ARTIFACT_ROOTS.get(relative.parts[0])
    if expected is None:
        fail(f"installer_artifact_root_unadmitted:{relative.parts[0]}")
    return expected


def require_expected_universal_root(input_root: Path, manifest_path: Path) -> None:
    try:
        relative = manifest_path.relative_to(input_root)
    except ValueError:
        fail(f"macos_universal_manifest_outside_input_root:{manifest_path}")
    if (
        len(relative.parts) != 2
        or relative.name != MACOS_UNIVERSAL_MANIFEST
        or relative.parts[0] != MACOS_UNIVERSAL_ARTIFACT_ROOT
    ):
        fail(f"macos_universal_manifest_location_invalid:{relative.as_posix()}")


def add_installer_manifest(
    manifest_path: Path,
    input_root: Path,
    version: str,
    version_root: Path,
    copied: dict[str, str],
    records: list[dict[str, Any]],
) -> None:
    data = load_json(manifest_path)
    if not isinstance(data, dict) or data.get("schema_id") != "scratchbird.installer_artifact_manifest.v1":
        fail(f"installer_manifest_schema_mismatch:{manifest_path}")
    platform = str(data.get("platform") or "")
    if platform not in {"linux", "windows", "macos"}:
        fail(f"installer_manifest_platform_invalid:{platform}")
    expected_platform, arch = require_expected_installer_root(input_root, manifest_path)
    if platform != expected_platform:
        fail(f"installer_manifest_artifact_root_platform_mismatch:{manifest_path}")
    if data.get("version") != version:
        fail(f"installer_manifest_version_mismatch:{manifest_path}")
    _admission, profile_digest = require_native_server_admission(data, manifest_path)
    if platform == "windows":
        windows = data.get("windows")
        if (
            not isinstance(windows, dict)
            or windows.get("package_mode") != "portable_zip_only"
            or windows.get("system_installer_included") is not False
            or windows.get("portable_archive_smoke_required") is not True
            or windows.get("native_default_port") != 3092
        ):
            fail(f"windows_zip_only_manifest_policy_invalid:{manifest_path}")
        rows = data.get("artifacts")
        if not isinstance(rows, list):
            fail(f"installer_manifest_artifacts_missing:{manifest_path}")
        artifact_names = []
        for row in rows:
            if not isinstance(row, dict) or not isinstance(row.get("path"), str):
                fail(f"installer_manifest_row_invalid:{manifest_path}")
            artifact_names.append(row["path"])
        if sum(name.endswith(".zip") for name in artifact_names) != 1:
            fail(f"windows_zip_only_zip_cardinality:{manifest_path}")
        for name in artifact_names:
            leaf = Path(name).name
            if (
                leaf == "WINDOWS_SYSTEM_PACKAGE_EVIDENCE.json"
                or Path(name).suffix.lower() in {".msi", ".wixpdb", ".wxs"}
            ):
                fail(f"windows_zip_only_forbidden_artifact:{manifest_path}:{name}")
    sources, archive = installer_files(manifest_path, data, platform)
    verify_published_native_archive(archive, profile_digest, platform, arch)
    for source in sources:
        reject_client_payload_identity(source.name, "installer_file")
        filename = safe_component(source.name, "installer_file")
        target = version_root / platform / arch / filename
        copy_file(
            source,
            target,
            version_root,
            copied,
            records,
            {
                "platform": platform,
                "architecture": arch,
                "source_manifest": INSTALLER_MANIFEST,
                "artifact_class": "installer_artifact",
            },
        )


def add_macos_universal_manifest(
    manifest_path: Path,
    input_root: Path,
    version: str,
    version_root: Path,
    copied: dict[str, str],
    records: list[dict[str, Any]],
) -> None:
    data = load_json(manifest_path)
    if not isinstance(data, dict) or data.get("schema_id") != "scratchbird.macos_universal_artifact_manifest.v1":
        fail(f"macos_universal_manifest_schema_mismatch:{manifest_path}")
    artifact = data.get("artifact")
    if not isinstance(artifact, dict):
        fail(f"macos_universal_artifact_missing:{manifest_path}")
    rel = artifact.get("path")
    if not isinstance(rel, str):
        fail(f"macos_universal_artifact_path_invalid:{manifest_path}")
    relative = safe_manifest_relative_path(rel, "macos_universal_artifact_path")
    rel = relative.as_posix()
    require_expected_universal_root(input_root, manifest_path)
    if data.get("version") != version:
        fail(f"macos_universal_manifest_version_mismatch:{manifest_path}")
    _admission, profile_digest = require_native_server_admission(data, manifest_path)
    artifact_path = manifest_path.parent / relative
    if not artifact_path.is_file() or artifact_path.is_symlink():
        fail(f"macos_universal_artifact_missing:{rel}")
    if artifact.get("sha256") != sha256_file(artifact_path):
        fail(f"macos_universal_sha256_mismatch:{rel}")
    require_exact_universal_root(manifest_path, rel)
    verify_published_native_archive(
        artifact_path,
        profile_digest,
        "macos",
        "universal",
        universal=True,
    )
    for source in (manifest_path, artifact_path):
        filename = safe_component(source.name, "macos_universal_file")
        target = version_root / "macos" / "universal" / filename
        copy_file(
            source,
            target,
            version_root,
            copied,
            records,
            {
                "platform": "macos",
                "architecture": "universal",
                "source_manifest": MACOS_UNIVERSAL_MANIFEST,
                "artifact_class": "installer_artifact",
            },
        )


def discover_manifests(input_root: Path) -> tuple[list[Path], list[Path]]:
    allowed_roots = set(INSTALLER_ARTIFACT_ROOTS) | {MACOS_UNIVERSAL_ARTIFACT_ROOT}
    for path in sorted(input_root.iterdir()):
        if not path.is_dir() or path.name not in allowed_roots:
            fail(f"web_input_artifact_root_unadmitted:{path.name}")
    installer = sorted(input_root.rglob(INSTALLER_MANIFEST))
    universal = sorted(input_root.rglob(MACOS_UNIVERSAL_MANIFEST))
    if not installer and not universal:
        fail(f"no_installer_manifests:{input_root}")
    seen_installer_roots: set[str] = set()
    for manifest in installer:
        platform, _arch = require_expected_installer_root(input_root, manifest)
        root = manifest.parent.name
        if root in seen_installer_roots:
            fail(f"installer_artifact_root_manifest_duplicate:{root}")
        seen_installer_roots.add(root)
        if root == MACOS_UNIVERSAL_ARTIFACT_ROOT:
            fail(f"installer_manifest_in_universal_artifact_root:{platform}")
    if len(universal) > 1:
        fail("macos_universal_manifest_cardinality")
    for manifest in universal:
        require_expected_universal_root(input_root, manifest)
    return installer, universal


def write_index(
    version_root: Path,
    channel: str,
    version: str,
    base_url: str | None,
    source_revision: str | None,
    run_id: str | None,
    records: list[dict[str, Any]],
) -> None:
    records.sort(key=lambda row: row["path"])
    manifest = {
        "schema_id": "scratchbird.web_distribution_manifest.v1",
        "generated_at_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
        "channel": channel,
        "version": version,
        "source_revision": source_revision,
        "github_run_id": run_id,
        "base_url": base_url,
        "publication_policy": "webserver_upload_only_no_github_release",
        "native_server_admission": {
            "distribution_surface": "scratchbird_native_no_emulation",
            "admission_controller": "native_server_only",
            "client_artifacts_permitted": False,
            "admitted_driver_adaptor_mcp_components": [],
            "dbeaver_hard_excluded": True,
        },
        "artifacts": records,
    }
    manifest_path = version_root / "WEB_DISTRIBUTION_MANIFEST.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    url_prefix = (base_url.rstrip("/") + f"/{channel}/{version}") if base_url else f"{channel}/{version}"
    (version_root / "UPLOAD_LAYOUT.txt").write_text(
        "\n".join(
            [
                "ScratchBird webserver package export",
                f"channel={channel}",
                f"version={version}",
                f"publication_policy=webserver_upload_only_no_github_release",
                f"upload_root={channel}/{version}",
                f"url_prefix={url_prefix}",
                "Upload this directory tree to the ScratchBird webserver after independent approval.",
                "",
            ]
        ),
        encoding="utf-8",
    )
    all_rows = [
        {
            "path": path.relative_to(version_root).as_posix(),
            "sha256": sha256_file(path),
        }
        for path in sorted(version_root.rglob("*"))
        if path.is_file() and path.name != "SHA256SUMS"
    ]
    (version_root / "SHA256SUMS").write_text(
        "".join(f"{row['sha256']}  {row['path']}\n" for row in all_rows),
        encoding="utf-8",
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-root", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--channel", required=True)
    parser.add_argument("--base-url")
    parser.add_argument("--source-revision")
    parser.add_argument("--github-run-id")
    args = parser.parse_args()

    input_root = args.input_root.resolve()
    if not input_root.is_dir():
        fail(f"input_root_missing:{input_root}")
    channel = sanitize_channel(args.channel)
    version = sanitize_version(args.version)
    output_root = args.output_root.resolve()
    if output_root.exists():
        shutil.rmtree(output_root)
    version_root = output_root / channel / version
    version_root.mkdir(parents=True)

    installer_manifests, universal_manifests = discover_manifests(input_root)
    copied: dict[str, str] = {}
    records: list[dict[str, Any]] = []
    for manifest in installer_manifests:
        add_installer_manifest(
            manifest,
            input_root,
            version,
            version_root,
            copied,
            records,
        )
    for manifest in universal_manifests:
        add_macos_universal_manifest(
            manifest,
            input_root,
            version,
            version_root,
            copied,
            records,
        )
    write_index(version_root, channel, version, args.base_url, args.source_revision, args.github_run_id, records)
    scan_private_text(output_root)
    print(f"create_web_distribution_bundle=passed:{version_root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
