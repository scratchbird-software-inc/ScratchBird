#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""CTest wrapper for CEIC-021 memory source gate fixtures and repo scan.

SEARCH_KEY: CEIC_021_MEMORY_SOURCE_GATE_TEST
"""

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys
import tempfile


def run(command: list[str], *, expect_success: bool) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, text=True, capture_output=True, check=False)
    if expect_success and result.returncode != 0:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise AssertionError(f"expected success: {' '.join(command)}")
    if not expect_success and result.returncode == 0:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise AssertionError(f"expected failure: {' '.join(command)}")
    return result


def empty_allowlist(path: pathlib.Path) -> pathlib.Path:
    path.write_text("[]\n", encoding="utf-8")
    return path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()

    repo_root = pathlib.Path(args.repo_root).resolve()
    gate = repo_root / "project/tools/ceic_memory_source_gate.py"

    with tempfile.TemporaryDirectory(prefix="ceic_021_memory_source_gate_") as temp_text:
        temp_dir = pathlib.Path(temp_text)
        allow_none = empty_allowlist(temp_dir / "empty_allowlist.json")

        repo_result = run(
            [
                sys.executable,
                str(gate),
                "--repo-root",
                str(repo_root),
            ],
            expect_success=True,
        )

        positive_result = run(
            [
                sys.executable,
                str(gate),
                "--repo-root",
                str(repo_root),
                "--scan-root",
                "project/tests/consolidated_enterprise/fixtures/ceic_021_memory_source_gate/positive",
                "--allowlist",
                str(allow_none),
            ],
            expect_success=True,
        )

        negative_result = run(
            [
                sys.executable,
                str(gate),
                "--repo-root",
                str(repo_root),
                "--scan-root",
                "project/tests/consolidated_enterprise/fixtures/ceic_021_memory_source_gate/negative",
                "--allowlist",
                str(allow_none),
            ],
            expect_success=False,
        )
        negative_output = negative_result.stdout + negative_result.stderr
        for category in (
            "direct_new",
            "direct_delete",
            "raw_malloc_free",
            "unbounded_growth",
            "raw_page_buffer",
            "global_allocator_mutex",
        ):
            if category not in negative_output:
                raise AssertionError(f"negative fixture did not trigger {category}")

        stale_allowlist = temp_dir / "stale_allowlist.json"
        stale_allowlist.write_text(
            json.dumps(
                [
                    {
                        "category": "direct_new",
                        "path": "project/tests/consolidated_enterprise/fixtures/ceic_021_memory_source_gate/negative/raw_hot_path_allocation.cpp",
                        "pattern": "new MissingType()",
                        "reason": "Deliberately stale CEIC-021 fixture entry proving the source gate rejects obsolete allowlist records.",
                    }
                ],
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
        stale_result = run(
            [
                sys.executable,
                str(gate),
                "--repo-root",
                str(repo_root),
                "--scan-root",
                "project/tests/consolidated_enterprise/fixtures/ceic_021_memory_source_gate/negative/raw_hot_path_allocation.cpp",
                "--allowlist",
                str(stale_allowlist),
            ],
            expect_success=False,
        )
        if "stale" not in (stale_result.stdout + stale_result.stderr):
            raise AssertionError("stale allowlist fixture did not report stale")

        broad_allowlist = temp_dir / "broad_allowlist.json"
        broad_allowlist.write_text(
            json.dumps(
                [
                    {
                        "category": "direct_new",
                        "path": "project/tests/consolidated_enterprise/fixtures/ceic_021_memory_source_gate/negative/raw_hot_path_allocation.cpp",
                        "pattern": "new",
                        "reason": "Deliberately broad CEIC-021 fixture entry proving the source gate rejects unsafe allowlist patterns.",
                    }
                ],
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
        broad_result = run(
            [
                sys.executable,
                str(gate),
                "--repo-root",
                str(repo_root),
                "--scan-root",
                "project/tests/consolidated_enterprise/fixtures/ceic_021_memory_source_gate/negative/raw_hot_path_allocation.cpp",
                "--allowlist",
                str(broad_allowlist),
            ],
            expect_success=False,
        )
        if "too broad" not in (broad_result.stdout + broad_result.stderr):
            raise AssertionError("broad allowlist fixture did not report too broad")

    print("ceic_021_memory_source_gate_test=pass")
    print(repo_result.stdout.strip())
    print(positive_result.stdout.strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
