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
import runpy
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
    sanitize = runpy.run_path(str(gate))["sanitize_cpp_lines"]
    lines = [
        "ordinary new Visible;",
        'auto x = "new Hidden"; new Visible; // delete hidden;',
        "/* new Hidden;",
        "free(hidden); */ delete visible;",
        'auto s = R"tag(new Hidden;',
        'delete Hidden; )tag"; new Visible;',
        "auto c = '\\''; new Visible;",
        'auto broken_raw = R"without_paren"; new Visible;',
    ]
    spans = [(0, 0), (9, 21), (0, 14), (0, 16), (9, 26), (0, 20), (9, 13), (19, 34)]
    expected = []
    for line, (begin, end) in zip(lines, spans):
        masked = line[:begin] + " " * (end - begin) + line[end:]
        if "//" in masked:
            start = masked.index("//")
            masked = masked[:start] + " " * (len(masked) - start)
        expected.append(masked)
    if sanitize(lines) != expected:
        raise AssertionError("comment/string scanner changed lexical state or source positions")

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

        # Every approved constructor is a single-site exception, not a file
        # exemption. An adjacent raw allocation must still be diagnosed.
        entries = json.loads((repo_root / "project/tools/ceic_memory_source_gate_allowlist.json").read_text())
        probe_root = temp_dir / "exact_factory_scope"
        for ordinal, entry in enumerate(entries):
            if entry["category"] != "direct_new":
                continue
            source = probe_root / entry["path"]
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_text(
                entry["pattern"] + ";\nnew UnapprovedAdjacentAllocation;\n",
                encoding="utf-8",
            )
            allow_one = temp_dir / f"factory_{ordinal}.json"
            allow_one.write_text(json.dumps([entry]), encoding="utf-8")
            result = run(
                [sys.executable, str(gate), "--repo-root", str(probe_root),
                 "--scan-root", entry["path"], "--allowlist", str(allow_one)],
                expect_success=False,
            )
            output = result.stdout + result.stderr
            if "UnapprovedAdjacentAllocation" not in output or "allowlist_errors=0" not in output:
                raise AssertionError(f"factory exception escaped its exact site: {entry['path']}")

    print("ceic_021_memory_source_gate_test=pass")
    print(repo_result.stdout.strip())
    print(positive_result.stdout.strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
