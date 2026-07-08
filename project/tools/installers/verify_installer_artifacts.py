#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Verify ScratchBird installer artifact manifests and boundary rules."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys


MANIFEST_NAME = "INSTALLER_ARTIFACT_MANIFEST.json"
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
REQUIRED_SUFFIXES = {
    "linux": (".tar.gz", ".deb"),
    "windows": (".zip",),
}


def fail(message: str) -> None:
    print(f"verify_installer_artifacts=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def is_text_candidate(path: Path) -> bool:
    return path.stat().st_size <= 2 * 1024 * 1024 and (
        path.suffix.lower() in {".json", ".md", ".txt", ".csv", ".xml", ".wxs", ".spec", ".service", ".sh", ".ps1"}
        or path.name in {"SHA256SUMS", "PKGBUILD"}
    )


def scan(root: Path) -> None:
    for path in sorted(item for item in root.rglob("*") if item.is_file()):
        rel = path.relative_to(root).as_posix()
        for fragment in FORBIDDEN_TEXT:
            if fragment in rel:
                fail(f"forbidden_path_fragment:{rel}:{fragment}")
        if not is_text_candidate(path):
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        for fragment in FORBIDDEN_TEXT:
            if fragment in text:
                fail(f"forbidden_text_fragment:{rel}:{fragment}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifact-root", type=Path, required=True)
    parser.add_argument("--platform", choices=("linux", "windows"), required=True)
    args = parser.parse_args()

    root = args.artifact_root.resolve()
    manifest_path = root / MANIFEST_NAME
    if not manifest_path.is_file():
        fail(f"missing_manifest:{manifest_path}")
    data = json.loads(manifest_path.read_text(encoding="utf-8"))
    if data.get("schema_id") != "scratchbird.installer_artifact_manifest.v1":
        fail("manifest_schema_mismatch")
    if data.get("platform") != args.platform:
        fail("manifest_platform_mismatch")
    artifacts = data.get("artifacts")
    if not isinstance(artifacts, list) or not artifacts:
        fail("manifest_artifacts_missing")
    paths = {row.get("path") for row in artifacts if isinstance(row, dict)}
    for suffix in REQUIRED_SUFFIXES[args.platform]:
        if not any(isinstance(path, str) and path.endswith(suffix) for path in paths):
            fail(f"required_artifact_missing:{suffix}")
    for row in artifacts:
        if not isinstance(row, dict):
            fail("manifest_row_not_object")
        rel = row.get("path")
        if not isinstance(rel, str):
            fail("manifest_row_path_invalid")
        path = root / rel
        if not path.is_file():
            fail(f"manifest_file_missing:{rel}")
        if row.get("bytes") != path.stat().st_size:
            fail(f"manifest_size_mismatch:{rel}")
        if row.get("sha256") != sha256_file(path):
            fail(f"manifest_sha256_mismatch:{rel}")
    scan(root)
    print(f"verify_installer_artifacts=passed:{root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
