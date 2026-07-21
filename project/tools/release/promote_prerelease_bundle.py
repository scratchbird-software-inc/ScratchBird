#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Materialize a self-verifying, metadata-only private pre-release attestation.

This command intentionally cannot promote a file payload.  A handwritten
"proof" or document cannot establish that a binary is a native ScratchBird
release, and accepting arbitrary text would allow an unadmitted client to be
relabeled as documentation.  Native tester artifacts must instead traverse
the independently verified installer/release-bundle route.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
from typing import Any


SCHEMA_ID = "scratchbird.prerelease_bundle_manifest.v1"
RELEASE_DATE_RE = re.compile(r"^[0-9]{4}[.][0-9]{2}[.][0-9]{2}$")
ALLOWED_TOP_LEVELS = {
    "docs",
    "installers",
    "proofs",
    "reference-parsers",
    "server",
    "source",
    "tools",
    "udr",
}
MAX_TEXT_PAYLOAD_BYTES = 2 * 1024 * 1024
ROOT_METADATA = {
    "RELEASE_MANIFEST.json",
    "SHA256SUMS",
}
ALLOWED_ROOT_FILES = ROOT_METADATA | {
    "FILE_LOCATION_MANIFEST.json",
    "README.md",
    "PRE_RELEASE_NOT_FINAL.txt",
}
MANUAL_PAYLOAD_COPY_POLICY = "manual_bundle_payload_copy_forbidden"


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[3]


def fail(message: str) -> None:
    print(f"promote_prerelease_bundle=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def git_text(repo_root: Path, *args: str) -> str | None:
    try:
        result = subprocess.run(
            ["git", *args],
            cwd=repo_root,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            check=False,
        )
    except OSError:
        return None
    if result.returncode != 0:
        return None
    return result.stdout.strip()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def ensure_safe_target(repo_root: Path, target: Path, release_date: str) -> Path:
    if not RELEASE_DATE_RE.match(release_date):
        fail(f"invalid_release_date:{release_date}:expected_YYYY.MM.DD")
    if target.is_symlink():
        fail(f"target_symlink_forbidden:{target}")
    resolved = target.resolve()
    expected_parent = (repo_root / "packaging").resolve()
    if expected_parent not in resolved.parents:
        fail(f"target_must_be_under_packaging:{resolved}")
    if resolved.name != release_date:
        fail(f"target_leaf_must_match_release_date:{resolved.name}:{release_date}")
    return resolved


def target_tree_entries(target_root: Path) -> list[Path]:
    """Enumerate a target tree without following a directory symlink."""

    entries: list[Path] = []

    def visit(directory: Path) -> None:
        try:
            children = sorted(os.scandir(directory), key=lambda entry: entry.name)
        except OSError as exc:
            fail(f"target_tree_read_failed:{directory}:{exc}")
        for child in children:
            path = Path(child.path)
            entries.append(path)
            if child.is_symlink():
                continue
            if child.is_dir(follow_symlinks=False):
                visit(path)

    visit(target_root)
    return entries


def require_metadata_text(path: Path, relative: str) -> None:
    """Reject links, executable files, and opaque bytes in root metadata."""

    if path.is_symlink():
        fail(f"target_symlink_forbidden:{relative}")
    try:
        mode = path.stat(follow_symlinks=False).st_mode
        contents = path.read_bytes()
    except OSError as exc:
        fail(f"target_metadata_read_failed:{relative}:{exc}")
    if not stat.S_ISREG(mode):
        fail(f"unsupported_target_type:{relative}")
    if mode & (stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH):
        fail(f"executable_payload_forbidden:target_metadata:{relative}")
    if len(contents) > MAX_TEXT_PAYLOAD_BYTES or b"\x00" in contents:
        fail(f"opaque_metadata_forbidden:{relative}")
    try:
        contents.decode("utf-8")
    except UnicodeDecodeError:
        fail(f"opaque_metadata_forbidden:{relative}")


def validate_target_tree(
    target_root: Path,
    *,
    require_categories: bool,
    required_root_files: set[str],
) -> None:
    """Accept only the generated metadata envelope and empty category dirs.

    Categories are retained as a visible declaration of what a future,
    independently admitted release route may contain.  They deliberately
    cannot contain files or nested directories in this manual route.
    """

    actual_categories: set[str] = set()
    actual_root_files: set[str] = set()
    for path in target_tree_entries(target_root):
        relative = path.relative_to(target_root)
        relative_text = relative.as_posix()
        if path.is_symlink():
            fail(f"target_symlink_forbidden:{relative_text}")
        if len(relative.parts) != 1:
            fail(f"manual_bundle_payload_entry_forbidden:{relative_text}")
        try:
            mode = path.stat(follow_symlinks=False).st_mode
        except OSError as exc:
            fail(f"target_stat_failed:{relative_text}:{exc}")
        if stat.S_ISDIR(mode):
            if relative_text not in ALLOWED_TOP_LEVELS:
                fail(f"unknown_target_category:{relative_text}")
            actual_categories.add(relative_text)
            continue
        if not stat.S_ISREG(mode):
            fail(f"unsupported_target_type:{relative_text}")
        if relative_text not in ALLOWED_ROOT_FILES:
            fail(f"unknown_target_root_file:{relative_text}")
        require_metadata_text(path, relative_text)
        actual_root_files.add(relative_text)
    if require_categories and actual_categories != ALLOWED_TOP_LEVELS:
        fail(
            "manual_bundle_category_set_mismatch:"
            f"missing={sorted(ALLOWED_TOP_LEVELS - actual_categories)}:"
            f"unexpected={sorted(actual_categories - ALLOWED_TOP_LEVELS)}"
        )
    missing_root_files = required_root_files - actual_root_files
    if missing_root_files:
        fail(f"manual_bundle_root_metadata_missing:{','.join(sorted(missing_root_files))}")


def write_default_docs(target_root: Path, release_date: str) -> None:
    """Write the complete generated text surface for this metadata attestation."""

    (target_root / "PRE_RELEASE_NOT_FINAL.txt").write_text(
        "ScratchBird private pre-release metadata attestation.\n"
        "This bundle is for internal/test coordination only and is not an official release.\n"
        "It contains no installable payload, driver, adaptor, MCP component, or DBeaver artifact.\n",
        encoding="utf-8",
    )
    (target_root / "README.md").write_text(
        f"# ScratchBird Pre-Release Metadata Attestation {release_date}\n\n"
        "This directory records a non-publishable, metadata-only manual pre-release state.\n"
        "Native tester artifacts must be generated through the verified installer release route.\n",
        encoding="utf-8",
    )
    for category in sorted(ALLOWED_TOP_LEVELS):
        (target_root / category).mkdir(parents=True, exist_ok=True)


def collect_files(target_root: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for path in sorted(item for item in target_root.rglob("*") if item.is_file()):
        relative = path.relative_to(target_root).as_posix()
        if relative in ROOT_METADATA:
            continue
        category = relative.split("/", 1)[0] if "/" in relative else "metadata"
        stat = path.stat()
        rows.append(
            {
                "path": relative,
                "category": category,
                "bytes": stat.st_size,
                "sha256": sha256_file(path),
            }
        )
    return rows


def write_file_location_manifest(target_root: Path, release_date: str) -> None:
    """Record the exact generated metadata files before release manifest hashing."""

    rows = []
    for row in collect_files(target_root):
        if row["path"] == "FILE_LOCATION_MANIFEST.json":
            continue
        rows.append(
            {
                "path": row["path"],
                "role": "generated_private_prerelease_metadata",
                "installer_destination_hint": "not_installable_metadata_attestation_only",
                "bytes": row["bytes"],
                "sha256": row["sha256"],
            }
        )
    manifest = {
        "schema_id": "scratchbird.prerelease_file_location_manifest.v1",
        "release_date": release_date,
        "files": rows,
    }
    (target_root / "FILE_LOCATION_MANIFEST.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def write_manifest(
    target_root: Path,
    release_date: str,
    channel: str,
    version: str | None,
    build_id: str | None,
    source_commit: str | None,
    source_dirty: bool,
) -> None:
    files = collect_files(target_root)
    manifest = {
        "schema_id": SCHEMA_ID,
        "channel": channel,
        "release_date": release_date,
        "version": version,
        "build_id": build_id,
        "generated_at_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
        "pre_release_not_final": True,
        "source": {
            "commit": source_commit,
            "dirty_before_promotion": source_dirty,
        },
        "policy": {
            "distribution": "private_pre_release",
            "official_release": False,
            "history_cleanup_required_before_public_release": True,
            "build_directory_is_disposable": True,
            "promotion_requires_explicit_command": True,
            "component_admission_controller": "native_server_only",
            "client_artifacts_permitted": False,
            "admitted_driver_adaptor_mcp_components": [],
            "dbeaver_hard_excluded": True,
            "manual_bundle_binary_payloads_permitted": False,
            "manual_bundle_payload_files_permitted": False,
            "manual_bundle_external_copy_permitted": False,
        },
        "categories": sorted(ALLOWED_TOP_LEVELS),
        "promoted_paths": [],
        "artifacts": files,
    }
    (target_root / "RELEASE_MANIFEST.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    sha_lines = [f"{row['sha256']}  {row['path']}" for row in files]
    (target_root / "SHA256SUMS").write_text("\n".join(sha_lines) + ("\n" if sha_lines else ""), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=repo_root_from_script())
    parser.add_argument("--release-date", required=True)
    parser.add_argument("--target", type=Path)
    parser.add_argument("--channel", default="prerelease")
    parser.add_argument("--version")
    parser.add_argument("--build-id")
    parser.add_argument(
        "--copy",
        action="append",
        default=[],
        metavar="SOURCE=DEST_REL",
        help="forbidden: this manual route cannot accept any payload file",
    )
    parser.add_argument(
        "--replace",
        action="store_true",
        help="forbidden with --copy; generated metadata is always refreshed",
    )
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    target = args.target or repo_root / "packaging" / args.release_date
    target_root = ensure_safe_target(repo_root, target, args.release_date)
    if args.channel != "prerelease":
        fail(f"unsupported_channel:{args.channel}")

    # There is deliberately no arbitrary-document exception.  A manual text
    # copy can be executable source code or a client implementation disguised
    # as a proof, and this tool has no independent payload authority.
    if args.copy or args.replace:
        fail(
            f"{MANUAL_PAYLOAD_COPY_POLICY}:"
            "use the verified native installer release pipeline"
        )

    source_commit = git_text(repo_root, "rev-parse", "HEAD")
    source_dirty = bool(git_text(repo_root, "status", "--porcelain"))

    target_root.mkdir(parents=True, exist_ok=True)
    validate_target_tree(
        target_root,
        require_categories=False,
        required_root_files=set(),
    )
    write_default_docs(target_root, args.release_date)
    validate_target_tree(
        target_root,
        require_categories=True,
        required_root_files={"README.md", "PRE_RELEASE_NOT_FINAL.txt"},
    )
    write_file_location_manifest(target_root, args.release_date)
    write_manifest(
        target_root,
        args.release_date,
        args.channel,
        args.version,
        args.build_id,
        source_commit,
        source_dirty,
    )
    validate_target_tree(
        target_root,
        require_categories=True,
        required_root_files=ALLOWED_ROOT_FILES,
    )
    print(f"promote_prerelease_bundle=pass:{target_root.relative_to(repo_root).as_posix()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
