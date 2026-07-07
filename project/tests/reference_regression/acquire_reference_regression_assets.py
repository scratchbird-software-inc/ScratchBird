#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Acquire scoped upstream regression-test payloads for reference parsers.

The public repository tracks this script and the source map only. Downloaded
upstream payloads stay under the ignored reference_release_acquisition tree,
and generated local evidence stays under build/.
"""

from __future__ import annotations

import argparse
import csv
import datetime as _dt
import hashlib
import pathlib
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass


REFERENCE_ROOT = pathlib.Path("project/tests/reference_regression")
SOURCE_MANIFEST = REFERENCE_ROOT / "reference_regression_acquisition_sources.csv"
ACQUISITION_ROOT = REFERENCE_ROOT / "reference_release_acquisition"
LOCAL_EVIDENCE_ROOT = pathlib.Path("build/reference_regression_acquisition")

SKIP_DIR_NAMES = {
    ".git",
    ".github",
    ".gradle",
    ".pytest_cache",
    ".tox",
    "__pycache__",
    "build",
    "cmake-build-debug",
    "cmake-build-release",
    "dist",
    "node_modules",
    "target",
}

SKIP_FILE_NAMES = {
    ".gitattributes",
    ".gitignore",
    ".gitmodules",
    "CODE_OF_CONDUCT.md",
    "CONTRIBUTING.md",
    "README",
    "README.adoc",
    "README.md",
    "README.rst",
    "SECURITY.md",
}

SKIP_FILE_SUFFIXES = {
    ".adoc",
    ".bmp",
    ".gif",
    ".jpeg",
    ".jpg",
    ".md",
    ".pdf",
    ".png",
    ".rst",
    ".svg",
}


@dataclass(frozen=True)
class AcquisitionSource:
    reference_id: str
    source_id: str
    display_name: str
    release_version: str
    repo_url: str
    upstream_ref: str
    acquisition_subdir: str
    selected_paths: tuple[str, ...]
    runner_tools: str
    official_reference_url: str
    policy_note: str

    @property
    def regression_root(self) -> pathlib.Path:
        return ACQUISITION_ROOT / self.acquisition_subdir / "regression"

    @property
    def payload_root(self) -> pathlib.Path:
        return self.regression_root / "acquired" / self.source_id

    @property
    def summary_manifest(self) -> pathlib.Path:
        return LOCAL_EVIDENCE_ROOT / "manifests" / f"{self.source_id.upper()}_ACQUISITION_MANIFEST.csv"


def run(cmd: list[str], cwd: pathlib.Path | None = None) -> None:
    subprocess.run(cmd, cwd=cwd, check=True)


def read_sources(repo: pathlib.Path) -> list[AcquisitionSource]:
    path = repo / SOURCE_MANIFEST
    with path.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    sources: list[AcquisitionSource] = []
    seen: set[str] = set()
    for row in rows:
        source_id = row["source_id"]
        if source_id in seen:
            raise AssertionError(f"duplicate acquisition source_id: {source_id}")
        seen.add(source_id)
        paths = tuple(part.strip() for part in row["selected_paths"].split(";") if part.strip())
        if not paths:
            raise AssertionError(f"{source_id}: no selected paths")
        sources.append(
            AcquisitionSource(
                reference_id=row["reference_id"],
                source_id=source_id,
                display_name=row["display_name"],
                release_version=row["release_version"],
                repo_url=row["repo_url"],
                upstream_ref=row["upstream_ref"],
                acquisition_subdir=row["acquisition_subdir"],
                selected_paths=paths,
                runner_tools=row["runner_tools"],
                official_reference_url=row["official_reference_url"],
                policy_note=row["policy_note"],
            )
        )
    return sources


def sparse_patterns(selected_paths: tuple[str, ...]) -> str:
    lines: list[str] = []
    for raw in selected_paths:
        path = raw.strip().strip("/")
        if not path:
            continue
        if path == ".":
            lines.append("/*")
            continue
        lines.append(f"/{path}")
        if not pathlib.PurePosixPath(path).suffix or "*" in path or "?" in path or "[" in path:
            lines.append(f"/{path}/**")
    return "\n".join(dict.fromkeys(lines)) + "\n"


def should_copy(path: pathlib.Path, rel: pathlib.Path) -> bool:
    if any(part in SKIP_DIR_NAMES for part in rel.parts):
        return False
    if path.name in SKIP_FILE_NAMES:
        return False
    if path.suffix.lower() in SKIP_FILE_SUFFIXES:
        return False
    return True


def copy_sparse_payload(worktree: pathlib.Path, payload_root: pathlib.Path) -> tuple[int, int, str]:
    if payload_root.exists():
        shutil.rmtree(payload_root)
    payload_root.mkdir(parents=True, exist_ok=True)

    files: list[tuple[pathlib.Path, int, str]] = []
    for source in sorted(worktree.rglob("*")):
        if not source.is_file():
            continue
        rel = source.relative_to(worktree)
        if rel.parts and rel.parts[0] == ".git":
            continue
        if not should_copy(source, rel):
            continue
        target = payload_root / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        digest = hashlib.sha256(target.read_bytes()).hexdigest()
        files.append((rel, target.stat().st_size, digest))

    if not files:
        raise AssertionError(f"{payload_root}: acquisition selected zero files")

    total_bytes = sum(size for _, size, _ in files)
    tree = hashlib.sha256()
    for rel, size, digest in files:
        tree.update(rel.as_posix().encode("utf-8"))
        tree.update(b"\0")
        tree.update(str(size).encode("ascii"))
        tree.update(b"\0")
        tree.update(digest.encode("ascii"))
        tree.update(b"\0")

    hash_manifest = payload_root / "SB_REFERENCE_ASSET_HASHES.csv"
    with hash_manifest.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(["relative_path", "bytes", "sha256"])
        for rel, size, digest in files:
            writer.writerow([rel.as_posix(), size, digest])

    return len(files), total_bytes, tree.hexdigest()


def write_scope_file(repo: pathlib.Path, source: AcquisitionSource) -> None:
    root = repo / source.regression_root
    root.mkdir(parents=True, exist_ok=True)
    scope = root / "PUBLIC_REGRESSION_SCOPE.md"
    if scope.exists():
        return
    scope.write_text(
        "# Public Regression Scope\n\n"
        "This directory is the ignored local acquisition area for reference "
        "parser regression tests and native replay tools. Downloaded payloads "
        "and generated summaries are not tracked by git.\n",
        encoding="utf-8",
    )


def write_summary(repo: pathlib.Path, source: AcquisitionSource, count: int, total: int, digest: str) -> None:
    path = repo / source.summary_manifest
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(
            [
                "source_id",
                "reference_id",
                "display_name",
                "release_version",
                "repo_url",
                "upstream_ref",
                "selected_paths",
                "runner_tools",
                "official_reference_url",
                "payload_root",
                "file_count",
                "total_bytes",
                "tree_shape_digest",
                "status",
                "generated_utc",
            ]
        )
        writer.writerow(
            [
                source.source_id,
                source.reference_id,
                source.display_name,
                source.release_version,
                source.repo_url,
                source.upstream_ref,
                ";".join(source.selected_paths),
                source.runner_tools,
                source.official_reference_url,
                source.payload_root.as_posix(),
                count,
                total,
                digest,
                "acquired",
                _dt.datetime.now(tz=_dt.UTC).strftime("%Y-%m-%dT%H:%M:%SZ"),
            ]
        )


def acquire_one(repo: pathlib.Path, source: AcquisitionSource, work_root: pathlib.Path) -> None:
    write_scope_file(repo, source)
    with tempfile.TemporaryDirectory(prefix=f"{source.source_id}.", dir=work_root) as tmp:
        worktree = pathlib.Path(tmp) / "repo"
        run(["git", "init", "--quiet", worktree.as_posix()])
        run(["git", "remote", "add", "origin", source.repo_url], cwd=worktree)
        run(["git", "config", "core.sparseCheckout", "true"], cwd=worktree)
        run(["git", "config", "core.sparseCheckoutCone", "false"], cwd=worktree)
        run(["git", "config", "remote.origin.promisor", "true"], cwd=worktree)
        run(["git", "config", "remote.origin.partialclonefilter", "blob:none"], cwd=worktree)
        info = worktree / ".git" / "info"
        info.mkdir(parents=True, exist_ok=True)
        (info / "sparse-checkout").write_text(sparse_patterns(source.selected_paths), encoding="utf-8")
        run(["git", "fetch", "--depth", "1", "origin", source.upstream_ref], cwd=worktree)
        run(["git", "checkout", "--quiet", "--force", "FETCH_HEAD"], cwd=worktree)
        count, total, digest = copy_sparse_payload(worktree, repo / source.payload_root)
    write_summary(repo, source, count, total, digest)
    print(
        f"acquired source_id={source.source_id} files={count} bytes={total} "
        f"digest={digest[:16]} payload={source.payload_root}"
    )


def read_summary(path: pathlib.Path) -> dict[str, str]:
    if not path.is_file():
        raise AssertionError(f"missing acquisition summary manifest: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    if len(rows) != 1:
        raise AssertionError(f"{path}: expected exactly one summary row")
    row = rows[0]
    if row.get("status") != "acquired":
        raise AssertionError(f"{path}: status is not acquired")
    if int(row.get("file_count", "0")) <= 0:
        raise AssertionError(f"{path}: file_count must be positive")
    if int(row.get("total_bytes", "0")) <= 0:
        raise AssertionError(f"{path}: total_bytes must be positive")
    if len(row.get("tree_shape_digest", "")) != 64:
        raise AssertionError(f"{path}: invalid tree_shape_digest")
    return row


def check_one(repo: pathlib.Path, source: AcquisitionSource, strict_payload: bool) -> None:
    summary = repo / source.summary_manifest
    row: dict[str, str] | None = None
    if summary.is_file():
        row = read_summary(summary)
        if row["source_id"] != source.source_id:
            raise AssertionError(f"{source.source_id}: summary source mismatch")
        if row["upstream_ref"] != source.upstream_ref:
            raise AssertionError(f"{source.source_id}: summary ref mismatch")
        if row["selected_paths"] != ";".join(source.selected_paths):
            raise AssertionError(f"{source.source_id}: selected path mismatch")
    elif not strict_payload:
        raise AssertionError(f"{source.source_id}: missing local summary manifest: {summary}")

    if strict_payload:
        payload = repo / source.payload_root
        if not payload.is_dir():
            raise AssertionError(f"{source.source_id}: payload root is missing: {payload}")
        files = [path for path in payload.rglob("*") if path.is_file() and path.name != "SB_REFERENCE_ASSET_HASHES.csv"]
        if not files:
            raise AssertionError(f"{source.source_id}: payload root has no files")
        if row is None:
            return
        if len(files) != int(row["file_count"]):
            raise AssertionError(f"{source.source_id}: payload file count mismatch")
        total = sum(path.stat().st_size for path in files)
        if total != int(row["total_bytes"]):
            raise AssertionError(f"{source.source_id}: payload byte count mismatch")


def selected_sources(sources: list[AcquisitionSource], wanted: set[str]) -> list[AcquisitionSource]:
    if not wanted:
        return sources
    selected = [source for source in sources if source.source_id in wanted or source.reference_id in wanted]
    missing = wanted - {source.source_id for source in selected} - {source.reference_id for source in selected}
    if missing:
        raise AssertionError(f"unknown acquisition source/reference: {', '.join(sorted(missing))}")
    return selected


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=pathlib.Path)
    parser.add_argument("--work-dir", type=pathlib.Path, default=pathlib.Path("/tmp/scratchbird_reference_acquisition"))
    parser.add_argument("--source", action="append", default=[], help="source_id or reference_id to process")
    parser.add_argument("--download", action="store_true")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--strict-payload", action="store_true")
    args = parser.parse_args(argv)

    repo = args.repo_root.resolve()
    sources = selected_sources(read_sources(repo), set(args.source))

    if not args.download and not args.check:
        parser.error("choose --download, --check, or both")

    if args.download:
        args.work_dir.mkdir(parents=True, exist_ok=True)
        for source in sources:
            acquire_one(repo, source, args.work_dir.resolve())

    if args.check:
        for source in sources:
            check_one(repo, source, args.strict_payload)
        strict = "yes" if args.strict_payload else "no"
        print(f"reference_regression_acquisition_check=pass sources={len(sources)} strict_payload={strict}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
