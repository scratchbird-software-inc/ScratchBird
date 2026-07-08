#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Reject committed or workflow-consumed temporary packaging dropbox content."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys


FORBIDDEN_WORKFLOW_TOKENS = (
    "packaging/",
    "packaging\\",
)


def fail(message: str) -> None:
    print(f"public_packaging_history_gate=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def git_lines(repo_root: Path, *args: str) -> list[str]:
    result = subprocess.run(
        ["git", *args],
        cwd=repo_root,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        return []
    return [line for line in result.stdout.splitlines() if line.strip()]


def check_gitignore(repo_root: Path) -> None:
    gitignore = repo_root / ".gitignore"
    if not gitignore.is_file():
        fail("missing_gitignore")
    text = gitignore.read_text(encoding="utf-8")
    for token in ("!/packaging/", "!/packaging/**"):
        if token in text:
            fail(f"packaging_unignore_rule_present:{token}")
    if "/packaging/" not in text:
        fail("packaging_ignore_rule_missing")


def check_workflows(repo_root: Path) -> None:
    workflow_root = repo_root / ".github" / "workflows"
    if not workflow_root.exists():
        return
    for path in sorted(workflow_root.glob("*.yml")) + sorted(workflow_root.glob("*.yaml")):
        text = path.read_text(encoding="utf-8")
        for token in FORBIDDEN_WORKFLOW_TOKENS:
            if token in text:
                fail(f"workflow_consumes_packaging:{path.relative_to(repo_root).as_posix()}:{token}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--allow-pending-removal", action="store_true")
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    packaging = repo_root / "packaging"
    if packaging.exists() and any(packaging.iterdir()):
        fail("packaging_directory_contains_files")

    tracked = git_lines(repo_root, "ls-files", "packaging")
    if tracked:
        if args.allow_pending_removal:
            deleted = set(git_lines(repo_root, "ls-files", "--deleted", "packaging"))
            if set(tracked) - deleted:
                fail(f"packaging_tracked_files_not_deleted:{len(set(tracked) - deleted)}")
        else:
            fail(f"packaging_tracked_files_present:{len(tracked)}")

    check_gitignore(repo_root)
    check_workflows(repo_root)
    print("public_packaging_history_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
