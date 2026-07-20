#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Exercise the webserver package export bundle generator."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        digest.update(handle.read())
    return digest.hexdigest()


def write_bytes(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def write_json(path: Path, data: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def make_installer_fixture(root: Path, platform: str, files: dict[str, bytes]) -> None:
    rows = []
    for rel, data in files.items():
        path = root / rel
        write_bytes(path, data)
        rows.append({"path": rel, "bytes": len(data), "sha256": sha256_file(path)})
    manifest = {
        "schema_id": "scratchbird.installer_artifact_manifest.v1",
        "platform": platform,
        "version": "1.2.3-beta",
        "build_id": "test-run",
        "artifacts": rows,
    }
    if platform == "windows":
        manifest["windows"] = {
            "package_mode": "portable_zip_only",
            "system_installer_included": False,
            "portable_archive_smoke_required": True,
            "native_default_port": 3092,
        }
    write_json(root / "INSTALLER_ARTIFACT_MANIFEST.json", manifest)
    (root / "SHA256SUMS").write_text(
        "".join(f"{row['sha256']}  {row['path']}\n" for row in rows),
        encoding="utf-8",
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, required=True)
    args = parser.parse_args()
    repo_root = args.repo_root.resolve()
    script = repo_root / "project" / "tools" / "installers" / "create_web_distribution_bundle.py"
    if not script.is_file():
        print(f"missing_script:{script}", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory(prefix="sb-webdist-test-") as temp:
        temp_root = Path(temp)
        input_root = temp_root / "input"
        output_root = temp_root / "output"
        make_installer_fixture(
            input_root / "scratchbird-linux-installers",
            "linux",
            {
                "scratchbird-linux-1.2.3-beta.tar.gz": b"linux tarball",
                "scratchbird_1.2.3_beta_amd64.deb": b"linux deb",
            },
        )
        make_installer_fixture(
            input_root / "scratchbird-windows-installers",
            "windows",
            {
                "scratchbird-windows-1.2.3-beta.zip": b"windows zip",
            },
        )
        universal_root = input_root / "scratchbird-macos-universal-installers"
        universal_artifact = universal_root / "scratchbird-macos-universal-1.2.3-beta.tar.gz"
        write_bytes(universal_artifact, b"macos universal tarball")
        write_json(
            universal_root / "MACOS_UNIVERSAL_ARTIFACT_MANIFEST.json",
            {
                "schema_id": "scratchbird.macos_universal_artifact_manifest.v1",
                "version": "1.2.3-beta",
                "build_id": "test-run",
                "artifact": {
                    "path": universal_artifact.name,
                    "bytes": universal_artifact.stat().st_size,
                    "sha256": sha256_file(universal_artifact),
                    "architectures": ["x86_64", "arm64"],
                    "status": "qa_universal_after_per_architecture_artifacts_verify",
                },
            },
        )

        completed = subprocess.run(
            [
                sys.executable,
                str(script),
                "--input-root",
                str(input_root),
                "--output-root",
                str(output_root),
                "--version",
                "1.2.3-beta",
                "--channel",
                "beta",
                "--base-url",
                "https://downloads.example.invalid",
                "--source-revision",
                "abc123",
                "--github-run-id",
                "42",
            ],
            cwd=repo_root,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        if completed.returncode != 0:
            print(completed.stdout, end="")
            return completed.returncode

        version_root = output_root / "beta" / "1.2.3-beta"
        expected = [
            version_root / "linux" / "x86_64" / "scratchbird-linux-1.2.3-beta.tar.gz",
            version_root / "linux" / "x86_64" / "scratchbird_1.2.3_beta_amd64.deb",
            version_root / "windows" / "x86_64" / "scratchbird-windows-1.2.3-beta.zip",
            version_root / "macos" / "universal" / "scratchbird-macos-universal-1.2.3-beta.tar.gz",
            version_root / "WEB_DISTRIBUTION_MANIFEST.json",
            version_root / "SHA256SUMS",
            version_root / "UPLOAD_LAYOUT.txt",
        ]
        missing = [path.relative_to(version_root).as_posix() for path in expected if not path.is_file()]
        if missing:
            print(f"missing_outputs:{missing}", file=sys.stderr)
            return 1
        manifest = json.loads((version_root / "WEB_DISTRIBUTION_MANIFEST.json").read_text(encoding="utf-8"))
        if manifest.get("publication_policy") != "webserver_upload_only_no_github_release":
            print("publication_policy_mismatch", file=sys.stderr)
            return 1
        paths = {row["path"] for row in manifest.get("artifacts", [])}
        required_paths = {
            "linux/x86_64/scratchbird-linux-1.2.3-beta.tar.gz",
            "windows/x86_64/scratchbird-windows-1.2.3-beta.zip",
            "macos/universal/scratchbird-macos-universal-1.2.3-beta.tar.gz",
        }
        if not required_paths.issubset(paths):
            print(f"manifest_paths_missing:{sorted(required_paths - paths)}", file=sys.stderr)
            return 1

        reject_input = temp_root / "reject-input"
        make_installer_fixture(
            reject_input / "scratchbird-windows-installers",
            "windows",
            {
                "scratchbird-windows-1.2.3-beta.zip": b"windows zip",
                "scratchbird-windows-1.2.3-beta.msi": b"forbidden msi",
            },
        )
        rejected = subprocess.run(
            [
                sys.executable,
                str(script),
                "--input-root",
                str(reject_input),
                "--output-root",
                str(temp_root / "reject-output"),
                "--version",
                "1.2.3-beta",
                "--channel",
                "beta",
            ],
            cwd=repo_root,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        if (
            rejected.returncode == 0
            or "windows_zip_only_forbidden_artifact" not in rejected.stdout
        ):
            print(f"windows_msi_web_export_not_rejected:{rejected.stdout}", file=sys.stderr)
            return 1
    print("web_distribution_bundle_test=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
