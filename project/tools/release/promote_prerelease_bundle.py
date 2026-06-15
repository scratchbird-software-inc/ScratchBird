#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Promote explicit build/doc/proof artifacts into a private pre-release bundle."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import glob
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
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
ROOT_METADATA = {
    "RELEASE_MANIFEST.json",
    "SHA256SUMS",
}
FORBIDDEN_SOURCE_FRAGMENTS = (
    "ScratchBird-Private",
    "/local_work/",
    "\\local_work\\",
)


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
    resolved = target.resolve()
    expected_parent = (repo_root / "packaging").resolve()
    if expected_parent not in resolved.parents:
        fail(f"target_must_be_under_packaging:{resolved}")
    if resolved.name != release_date:
        fail(f"target_leaf_must_match_release_date:{resolved.name}:{release_date}")
    return resolved


def reject_private_source(path: Path) -> None:
    value = path.resolve().as_posix()
    for fragment in FORBIDDEN_SOURCE_FRAGMENTS:
        if fragment in value:
            fail(f"private_source_not_allowed:{value}")


def safe_relative_dest(value: str) -> tuple[Path, bool]:
    is_directory_hint = value.endswith("/")
    dest = Path(value.strip("/"))
    if dest.is_absolute() or ".." in dest.parts or not dest.parts:
        fail(f"invalid_destination:{value}")
    if dest.parts[0] not in ALLOWED_TOP_LEVELS:
        fail(f"destination_must_start_with_known_category:{value}")
    return dest, is_directory_hint


def copy_file(source: Path, target: Path, replace: bool) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists():
        if not replace:
            fail(f"target_exists:{target}")
        if target.is_dir():
            shutil.rmtree(target)
        else:
            target.unlink()
    shutil.copy2(source, target)


def copy_directory_contents(source: Path, target_dir: Path, replace: bool) -> None:
    if target_dir.exists() and not target_dir.is_dir():
        if not replace:
            fail(f"target_exists_not_directory:{target_dir}")
        target_dir.unlink()
    target_dir.mkdir(parents=True, exist_ok=True)
    for child in sorted(source.iterdir()):
        target = target_dir / child.name
        if child.is_dir():
            if target.exists():
                if not replace:
                    fail(f"target_exists:{target}")
                shutil.rmtree(target)
            shutil.copytree(child, target)
        elif child.is_file():
            copy_file(child, target, replace)


def expand_sources(raw_source: str, repo_root: Path) -> list[Path]:
    candidate = Path(raw_source).expanduser()
    pattern = candidate.as_posix() if candidate.is_absolute() else (repo_root / candidate).as_posix()
    if any(token in pattern for token in "*?["):
        paths = [Path(item) for item in sorted(glob.glob(pattern))]
    else:
        paths = [Path(pattern)]
    if not paths:
        fail(f"source_not_found:{raw_source}")
    for path in paths:
        if not path.exists():
            fail(f"source_not_found:{raw_source}")
        reject_private_source(path)
    return paths


def promote_copy(copy_arg: str, repo_root: Path, target_root: Path, replace: bool) -> list[str]:
    if "=" not in copy_arg:
        fail(f"invalid_copy_arg:{copy_arg}:expected_source=destination")
    raw_source, raw_dest = copy_arg.split("=", 1)
    sources = expand_sources(raw_source, repo_root)
    dest_rel, directory_hint = safe_relative_dest(raw_dest)
    copied: list[str] = []
    multi_source = len(sources) > 1
    for source in sources:
        if source.is_dir():
            destination = target_root / dest_rel
            copy_directory_contents(source, destination, replace)
            copied.append(destination.relative_to(target_root).as_posix() + "/")
            continue
        if not source.is_file():
            fail(f"unsupported_source_type:{source}")
        if multi_source or directory_hint or not dest_rel.suffix:
            destination = target_root / dest_rel / source.name
        else:
            destination = target_root / dest_rel
        copy_file(source, destination, replace)
        copied.append(destination.relative_to(target_root).as_posix())
    return copied


def write_default_docs(target_root: Path, release_date: str) -> None:
    prerelease = target_root / "PRE_RELEASE_NOT_FINAL.txt"
    if not prerelease.exists():
        prerelease.write_text(
            "ScratchBird private pre-release bundle.\n"
            "This bundle is for internal/test distribution only and is not an official release.\n"
            "Artifacts may be replaced before public release approval.\n",
            encoding="utf-8",
        )
    readme = target_root / "README.md"
    if not readme.exists():
        readme.write_text(
            f"# ScratchBird Pre-Release Bundle {release_date}\n\n"
            "This directory is an explicitly promoted private pre-release bundle.\n"
            "Only finalized candidate artifacts belong here.\n",
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


def write_manifest(
    repo_root: Path,
    target_root: Path,
    release_date: str,
    channel: str,
    version: str | None,
    build_id: str | None,
    copied: list[str],
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
        },
        "categories": sorted(ALLOWED_TOP_LEVELS),
        "promoted_paths": sorted(copied),
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
    parser.add_argument("--copy", action="append", default=[], metavar="SOURCE=DEST_REL")
    parser.add_argument("--replace", action="store_true")
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    target = args.target or repo_root / "packaging" / args.release_date
    target_root = ensure_safe_target(repo_root, target, args.release_date)
    if args.channel != "prerelease":
        fail(f"unsupported_channel:{args.channel}")

    source_commit = git_text(repo_root, "rev-parse", "HEAD")
    source_dirty = bool(git_text(repo_root, "status", "--porcelain"))

    target_root.mkdir(parents=True, exist_ok=True)
    write_default_docs(target_root, args.release_date)
    copied: list[str] = []
    for copy_arg in args.copy:
        copied.extend(promote_copy(copy_arg, repo_root, target_root, args.replace))
    write_manifest(
        repo_root,
        target_root,
        args.release_date,
        args.channel,
        args.version,
        args.build_id,
        copied,
        source_commit,
        source_dirty,
    )
    print(f"promote_prerelease_bundle=pass:{target_root.relative_to(repo_root).as_posix()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
