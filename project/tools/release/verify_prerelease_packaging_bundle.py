#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Verify a private ScratchBird pre-release packaging bundle."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
from typing import Any


SCHEMA_ID = "scratchbird.prerelease_bundle_manifest.v1"
RELEASE_DATE_RE = re.compile(r"^[0-9]{4}[.][0-9]{2}[.][0-9]{2}$")
ALLOWED_TOP_LEVELS = {
    "docs",
    "drivers",
    "installers",
    "proofs",
    "reference-parsers",
    "server",
    "source",
    "tools",
    "udr",
}
REQUIRED_ROOT_FILES = (
    "README.md",
    "PRE_RELEASE_NOT_FINAL.txt",
    "RELEASE_MANIFEST.json",
    "SHA256SUMS",
)
FORBIDDEN_PATH_PARTS = {
    ".git",
    ".staging",
    "__pycache__",
    ".pytest_cache",
    "CMakeFiles",
    "Testing",
}
FORBIDDEN_TEXT_FRAGMENTS = (
    "ScratchBird-Private",
    "/home/dcalford",
    "local_work",
)
CHECKSUM_EXCLUDE = {
    "RELEASE_MANIFEST.json",
    "SHA256SUMS",
}


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[3]


def fail(message: str) -> None:
    print(f"verify_prerelease_packaging_bundle=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def is_text_candidate(path: Path) -> bool:
    if path.stat().st_size > 2 * 1024 * 1024:
        return False
    if path.suffix.lower() in {
        ".json",
        ".md",
        ".txt",
        ".csv",
        ".yaml",
        ".yml",
        ".xml",
        ".ini",
        ".conf",
        ".ps1",
        ".sh",
        ".py",
    }:
        return True
    return path.name in {"SHA256SUMS", "PKGBUILD"}


def load_json(path: Path) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        fail(f"manifest_invalid:{exc}")
    if not isinstance(data, dict):
        fail("manifest_not_object")
    return data


def parse_sha256sums(path: Path) -> dict[str, str]:
    rows: dict[str, str] = {}
    text = path.read_text(encoding="utf-8")
    for lineno, line in enumerate(text.splitlines(), start=1):
        if not line.strip():
            continue
        if "  " not in line:
            fail(f"sha256sums_invalid_line:{lineno}")
        digest, rel = line.split("  ", 1)
        if not re.fullmatch(r"[0-9a-f]{64}", digest):
            fail(f"sha256sums_invalid_digest:{lineno}")
        if rel in rows:
            fail(f"sha256sums_duplicate:{rel}")
        rows[rel] = digest
    return rows


def collect_files(root: Path) -> dict[str, Path]:
    files: dict[str, Path] = {}
    for path in sorted(item for item in root.rglob("*") if item.is_file()):
        rel = path.relative_to(root).as_posix()
        if rel in CHECKSUM_EXCLUDE:
            continue
        files[rel] = path
    return files


def validate_root(repo_root: Path, release_root: Path) -> str:
    resolved = release_root.resolve()
    packaging_root = (repo_root / "packaging").resolve()
    if packaging_root not in resolved.parents:
        fail(f"release_root_not_under_packaging:{resolved}")
    release_date = resolved.name
    if not RELEASE_DATE_RE.match(release_date):
        fail(f"invalid_release_date_directory:{release_date}")
    return release_date


def scan_paths_and_text(release_root: Path) -> None:
    for path in sorted(item for item in release_root.rglob("*") if item.is_file()):
        rel = path.relative_to(release_root).as_posix()
        if any(part in FORBIDDEN_PATH_PARTS for part in path.relative_to(release_root).parts):
            fail(f"forbidden_path_part:{rel}")
        top = rel.split("/", 1)[0]
        if "/" in rel and top not in ALLOWED_TOP_LEVELS:
            fail(f"unknown_top_level_category:{rel}")
        if not is_text_candidate(path):
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        for fragment in FORBIDDEN_TEXT_FRAGMENTS:
            if fragment in text:
                fail(f"private_reference_fragment:{rel}:{fragment}")


def validate_manifest(
    release_root: Path,
    release_date: str,
    manifest: dict[str, Any],
    files: dict[str, Path],
    allow_empty: bool,
) -> None:
    if manifest.get("schema_id") != SCHEMA_ID:
        fail("manifest_schema_mismatch")
    if manifest.get("channel") != "prerelease":
        fail("manifest_channel_must_be_prerelease")
    if manifest.get("release_date") != release_date:
        fail("manifest_release_date_mismatch")
    if manifest.get("pre_release_not_final") is not True:
        fail("manifest_missing_prerelease_flag")
    policy = manifest.get("policy")
    if not isinstance(policy, dict) or policy.get("official_release") is not False:
        fail("manifest_official_release_policy_invalid")
    if policy.get("history_cleanup_required_before_public_release") is not True:
        fail("manifest_missing_history_cleanup_policy")
    categories = set(manifest.get("categories", []))
    if categories != ALLOWED_TOP_LEVELS:
        fail("manifest_categories_mismatch")
    artifact_rows = manifest.get("artifacts")
    if not isinstance(artifact_rows, list):
        fail("manifest_artifacts_not_list")
    manifest_paths: set[str] = set()
    for row in artifact_rows:
        if not isinstance(row, dict):
            fail("manifest_artifact_not_object")
        rel = row.get("path")
        if not isinstance(rel, str) or rel not in files:
            fail(f"manifest_artifact_missing_file:{rel}")
        if rel in manifest_paths:
            fail(f"manifest_artifact_duplicate:{rel}")
        manifest_paths.add(rel)
        if row.get("sha256") != sha256_file(files[rel]):
            fail(f"manifest_artifact_checksum_mismatch:{rel}")
        if row.get("bytes") != files[rel].stat().st_size:
            fail(f"manifest_artifact_size_mismatch:{rel}")
    if set(files) != manifest_paths:
        missing = sorted(set(files) - manifest_paths)
        extra = sorted(manifest_paths - set(files))
        if missing:
            fail(f"manifest_missing_files:{','.join(missing[:10])}")
        if extra:
            fail(f"manifest_extra_files:{','.join(extra[:10])}")
    payload_files = [
        rel for rel in files
        if not rel.endswith("/README.md") and rel not in {"README.md", "PRE_RELEASE_NOT_FINAL.txt"}
    ]
    if not allow_empty and not payload_files:
        fail("bundle_has_no_payload_files")


def validate_sha256sums(rows: dict[str, str], files: dict[str, Path]) -> None:
    if set(rows) != set(files):
        missing = sorted(set(files) - set(rows))
        extra = sorted(set(rows) - set(files))
        if missing:
            fail(f"sha256sums_missing_files:{','.join(missing[:10])}")
        if extra:
            fail(f"sha256sums_extra_files:{','.join(extra[:10])}")
    for rel, path in files.items():
        if rows[rel] != sha256_file(path):
            fail(f"sha256sums_checksum_mismatch:{rel}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("release_root", type=Path)
    parser.add_argument("--repo-root", type=Path, default=repo_root_from_script())
    parser.add_argument("--allow-empty", action="store_true")
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    release_root = args.release_root.resolve()
    release_date = validate_root(repo_root, release_root)
    for rel in REQUIRED_ROOT_FILES:
        if not (release_root / rel).is_file():
            fail(f"missing_root_file:{rel}")
    marker = (release_root / "PRE_RELEASE_NOT_FINAL.txt").read_text(encoding="utf-8").lower()
    if "pre-release" not in marker and "pre release" not in marker:
        fail("prerelease_marker_missing_text")
    for category in sorted(ALLOWED_TOP_LEVELS):
        if not (release_root / category).is_dir():
            fail(f"missing_category_dir:{category}")
    scan_paths_and_text(release_root)
    files = collect_files(release_root)
    manifest = load_json(release_root / "RELEASE_MANIFEST.json")
    validate_manifest(release_root, release_date, manifest, files, args.allow_empty)
    validate_sha256sums(parse_sha256sums(release_root / "SHA256SUMS"), files)
    print(f"verify_prerelease_packaging_bundle=pass:{release_root.relative_to(repo_root).as_posix()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
