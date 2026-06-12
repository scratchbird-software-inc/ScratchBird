#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Harness for the SBsql multilingual resource edge-safety release gate."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument(
        "--tool",
        type=Path,
        default=None,
        help="override the release gate path",
    )
    parser.add_argument("--verbose", action="store_true", help="print evidence anchors from the gate")
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    tool = args.tool
    if tool is None:
        tool = repo_root / "project/tools/release/sbsql_multilingual_resource_edge_safety_gate.py"
    tool = tool.resolve()

    if not repo_root.is_dir():
        print(f"sbsql_multilingual_resource_edge_safety_harness=fail: repo root not found: {repo_root}", file=sys.stderr)
        return 2
    if not tool.is_file():
        print(f"sbsql_multilingual_resource_edge_safety_harness=fail: gate not found: {tool}", file=sys.stderr)
        return 2

    command = [sys.executable, "-B", str(tool), "--repo-root", str(repo_root)]
    if args.verbose:
        command.append("--verbose")

    result = subprocess.run(
        command,
        cwd=repo_root,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.stdout:
        print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    if result.returncode != 0:
        print(
            "sbsql_multilingual_resource_edge_safety_harness=fail:"
            f" gate_exit_code={result.returncode}",
            file=sys.stderr,
        )
        return result.returncode

    print("sbsql_multilingual_resource_edge_safety_harness=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
