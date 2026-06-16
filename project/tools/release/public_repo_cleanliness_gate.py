#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Fail public release checks when WIP material leaks into product areas."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import sys


ALLOWED_PREFIXES = (
    "docs/documentation/draft/",
    "project/tests/release_evidence/",
    "project/tests/reference_regression/reference_release_acquisition/",
)

ALLOWED_PARTS = (
    "/fixtures/",
    "/artifacts/",
)

WIP_BASENAME_PATTERNS = (
    re.compile(r"^S\d+_.*IMPLEMENTATION\.md$", re.IGNORECASE),
    re.compile(r"^TRACKER\.csv$", re.IGNORECASE),
    re.compile(r"^CLOSURE_REPORT\.md$", re.IGNORECASE),
    re.compile(r"^FINAL_AUDIT\.md$", re.IGNORECASE),
    re.compile(r".*_WORKPLAN\.(md|csv|txt)$", re.IGNORECASE),
    re.compile(r".*_TRACKER\.(md|csv|txt)$", re.IGNORECASE),
    re.compile(r".*_AUDIT\.md$", re.IGNORECASE),
    re.compile(r".*_REPORT\.md$", re.IGNORECASE),
)

WIP_PATH_PATTERNS = (
    re.compile(r"(^|/)workplans?(/|$)", re.IGNORECASE),
    re.compile(r"(^|/)audit_reports?(/|$)", re.IGNORECASE),
)


def repo_files(repo_root: Path) -> list[str]:
    try:
        completed = subprocess.run(
            ["git", "ls-files"],
            cwd=repo_root,
            check=True,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except (OSError, subprocess.CalledProcessError) as exc:
        raise RuntimeError(f"git ls-files failed: {exc}") from exc
    return [line.strip() for line in completed.stdout.splitlines() if line.strip()]


def is_allowed(path: str) -> bool:
    return path.startswith(ALLOWED_PREFIXES) or any(part in path for part in ALLOWED_PARTS)


def is_wip(path: str) -> bool:
    if is_allowed(path):
        return False
    basename = Path(path).name
    if any(pattern.search(path) for pattern in WIP_PATH_PATTERNS):
        return True
    return any(pattern.match(basename) for pattern in WIP_BASENAME_PATTERNS)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    args = parser.parse_args()

    repo_root = args.root.resolve()
    offenders = [path for path in repo_files(repo_root) if is_wip(path)]
    if offenders:
        print("public_repo_cleanliness_gate=failed", file=sys.stderr)
        for path in offenders:
            print(f"public_wip_material:{path}", file=sys.stderr)
        return 1

    print("public_repo_cleanliness_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
