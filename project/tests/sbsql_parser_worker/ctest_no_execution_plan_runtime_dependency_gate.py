#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Fail if CTest runtime commands depend on execution_plan directories."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys


FORBIDDEN = (
    "docs" + "/execution-plans",
    "docs" + "/completed-execution-plans",
    "--execution_plan-root",
    "--closed-execution_plan-root",
    "--closure-execution_plan-root",
)


def scan_file(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8", errors="replace")
    errors: list[str] = []
    for token in FORBIDDEN:
        if token in text:
            errors.append(f"{path}: contains {token}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--build-root", required=True)
    args = parser.parse_args()

    repo_root = Path(args.repo_root).resolve()
    build_root = Path(args.build_root).resolve()

    paths: list[Path] = []
    paths.extend((repo_root / "project").rglob("CMakeLists.txt"))
    paths.extend((repo_root / "project").rglob("*.cmake"))
    if build_root.exists():
        paths.extend(build_root.rglob("CTestTestfile.cmake"))

    errors: list[str] = []
    for path in sorted(set(paths)):
        if any(part == "fixtures" for part in path.parts):
            continue
        errors.extend(scan_file(path))

    if errors:
        for error in errors:
            print(f"CTEST_EXECUTION_PLAN_RUNTIME_DEPENDENCY=failed: {error}", file=sys.stderr)
        return 1

    print("CTEST_EXECUTION_PLAN_RUNTIME_DEPENDENCY=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
