#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""CTest wrapper for CEIC-022 static memory invariant and lane gates.

SEARCH_KEY: CEIC_022_MEMORY_INVARIANT_GATE_TEST
"""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys


EXPECTED_CATEGORIES = (
    "allocation_without_scope",
    "query_grant_without_context",
    "protected_memory_without_zero_on_release",
    "page_cache_frame_without_memory_manager_ownership",
    "spill_without_unified_budget_reservation",
    "reservation_without_release_cleanup",
    "support_bundle_protected_material_copy",
    "raw_page_buffer_allocation",
    "forbidden_authority_claim",
    "production_fixture_hook_leak",
)


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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()
    repo_root = pathlib.Path(args.repo_root).resolve()
    gate = repo_root / "project/tools/ceic_memory_invariant_gate.py"
    positive_root = (
        "project/tests/consolidated_enterprise/fixtures/ceic_022_memory_invariants/positive"
    )
    negative_root = (
        "project/tests/consolidated_enterprise/fixtures/ceic_022_memory_invariants/negative"
    )

    repo_result = run(
        [sys.executable, str(gate), "--repo-root", str(repo_root)],
        expect_success=True,
    )
    positive_result = run(
        [
            sys.executable,
            str(gate),
            "--repo-root",
            str(repo_root),
            "--scan-root",
            positive_root,
        ],
        expect_success=True,
    )
    negative_command = [
        sys.executable,
        str(gate),
        "--repo-root",
        str(repo_root),
        "--scan-root",
        negative_root,
    ]
    for category in EXPECTED_CATEGORIES:
        negative_command.extend(["--expect-category", category])
    negative_result = run(negative_command, expect_success=False)
    negative_output = negative_result.stdout + negative_result.stderr
    for category in EXPECTED_CATEGORIES:
        if category not in negative_output:
            raise AssertionError(f"negative CEIC-022 fixture did not trigger {category}")

    print("ceic_022_memory_invariant_gate_test=pass")
    print(repo_result.stdout.strip())
    print(positive_result.stdout.strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
