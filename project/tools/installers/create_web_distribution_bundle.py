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
from pathlib import Path
import shutil
import sys
from typing import Any


INSTALLER_MANIFEST = "INSTALLER_ARTIFACT_MANIFEST.json"
MACOS_UNIVERSAL_MANIFEST = "MACOS_UNIVERSAL_ARTIFACT_MANIFEST.json"
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


def reject_public_path(value: str, context: str) -> None:
    if Path(value).is_absolute():
        fail(f"absolute_path_forbidden:{context}:{value}")
    for fragment in FORBIDDEN_TEXT:
        if fragment in value:
            fail(f"forbidden_path_fragment:{context}:{fragment}:{value}")


def safe_component(value: str, context: str) -> str:
    if not value or value in {".", ".."} or "/" in value or "\\" in value:
        fail(f"unsafe_component:{context}:{value}")
    reject_public_path(value, context)
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
        if not is_text_candidate(path):
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        for fragment in FORBIDDEN_TEXT:
            if fragment in text:
                fail(f"forbidden_text_fragment:{rel}:{fragment}")


def arch_segment(platform: str, manifest_path: Path) -> str:
    parts = {part.lower() for part in manifest_path.parts}
    joined = "/".join(part.lower() for part in manifest_path.parts)
    if platform == "macos":
        if "universal" in joined:
            return "universal"
        if "arm64" in parts or "macos-arm64" in parts:
            return "arm64"
        if "x86_64" in parts or "x64" in parts or "macos-x86_64" in parts:
            return "x86_64"
        return "unknown-arch"
    if platform in {"linux", "windows"}:
        return "x86_64"
    return "unknown-arch"


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


def installer_files(manifest_path: Path, data: dict[str, Any]) -> list[Path]:
    root = manifest_path.parent
    files = [manifest_path]
    sha_path = root / "SHA256SUMS"
    if sha_path.is_file():
        files.append(sha_path)
    artifacts = data.get("artifacts")
    if not isinstance(artifacts, list):
        fail(f"installer_manifest_artifacts_missing:{manifest_path}")
    for row in artifacts:
        if not isinstance(row, dict):
            fail(f"installer_manifest_row_invalid:{manifest_path}")
        rel = row.get("path")
        if not isinstance(rel, str):
            fail(f"installer_manifest_row_path_invalid:{manifest_path}")
        reject_public_path(rel, "installer_manifest_path")
        path = root / rel
        if not path.is_file():
            fail(f"installer_manifest_file_missing:{rel}")
        expected = row.get("sha256")
        actual = sha256_file(path)
        if expected != actual:
            fail(f"installer_manifest_sha256_mismatch:{rel}")
        files.append(path)
    return files


def add_installer_manifest(
    manifest_path: Path,
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
    arch = arch_segment(platform, manifest_path)
    for source in installer_files(manifest_path, data):
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
    reject_public_path(rel, "macos_universal_artifact_path")
    artifact_path = manifest_path.parent / rel
    if artifact.get("sha256") != sha256_file(artifact_path):
        fail(f"macos_universal_sha256_mismatch:{rel}")
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
    installer = sorted(input_root.rglob(INSTALLER_MANIFEST))
    universal = sorted(input_root.rglob(MACOS_UNIVERSAL_MANIFEST))
    if not installer and not universal:
        fail(f"no_installer_manifests:{input_root}")
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
        add_installer_manifest(manifest, version_root, copied, records)
    for manifest in universal_manifests:
        add_macos_universal_manifest(manifest, version_root, copied, records)
    write_index(version_root, channel, version, args.base_url, args.source_revision, args.github_run_id, records)
    scan_private_text(output_root)
    print(f"create_web_distribution_bundle=passed:{version_root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
