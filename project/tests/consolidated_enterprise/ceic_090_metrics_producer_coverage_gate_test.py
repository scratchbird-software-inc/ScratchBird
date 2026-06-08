#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""CTest wrapper for CEIC-090 metrics producer coverage.

SEARCH_KEY: CEIC_090_METRICS_PRODUCER_COVERAGE_GATE_TEST
"""

from __future__ import annotations

import argparse
import csv
import pathlib
import subprocess
import sys
import tempfile
from collections.abc import Callable


MATRIX = pathlib.Path(
    "docs" "/completed-execution-plans/consolidated-enterprise-proof-implementation-closure/"
    "METRICS_PRODUCER_COVERAGE_MATRIX.csv"
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


def read_rows(path: pathlib.Path) -> tuple[list[str], list[dict[str, str]]]:
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        return list(reader.fieldnames or []), [{key: value or "" for key, value in row.items()} for row in reader]


def write_rows(path: pathlib.Path, fieldnames: list[str], rows: list[dict[str, str]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames, quoting=csv.QUOTE_ALL)
        writer.writeheader()
        writer.writerows(rows)


def mutate(
    source: pathlib.Path,
    destination: pathlib.Path,
    metric_family: str,
    change: Callable[[dict[str, str]], None],
) -> None:
    fieldnames, rows = read_rows(source)
    for row in rows:
        if row["metric_family"] == metric_family:
            change(row)
            write_rows(destination, fieldnames, rows)
            return
    raise AssertionError(f"missing metric row: {metric_family}")


def expect_failure_contains(command: list[str], text: str) -> None:
    result = run(command, expect_success=False)
    output = result.stdout + result.stderr
    if text not in output:
        raise AssertionError(f"expected failure output to contain {text!r}, got: {output}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()

    repo_root = pathlib.Path(args.repo_root).resolve()
    tool = repo_root / "project/tools/ceic_metrics_producer_coverage.py"
    matrix = repo_root / MATRIX
    command = [sys.executable, str(tool), "--repo-root", str(repo_root)]

    positive = run(command, expect_success=True)

    with tempfile.TemporaryDirectory(prefix="ceic_090_metrics_") as temp_text:
        temp_dir = pathlib.Path(temp_text)

        cases: list[tuple[str, str, str, Callable[[dict[str, str]], None]]] = [
            (
                "descriptor_only",
                "memory_allocation_bytes",
                "descriptor_only",
                lambda row: row.update({"status": "descriptor_only_complete"}),
            ),
            (
                "missing_producer",
                "memory_context_peak_bytes",
                "missing_producer",
                lambda row: row.update({"required_producer_path": ""}),
            ),
            (
                "static_only",
                "index_generation_publish",
                "static_only",
                lambda row: row.update({"status": "static_only_complete"}),
            ),
            (
                "stale",
                "optimizer_estimate_actual_error",
                "stale_artifact",
                lambda row: row.update({"status": "stale_artifact"}),
            ),
            (
                "placeholder_support",
                "support_bundle_generation",
                "placeholder_evidence",
                lambda row: row.update({"support_bundle_path": "placeholder support-bundle production evidence"}),
            ),
            (
                "local_cluster",
                "agent_tenant_coordination",
                "local_cluster_claim",
                lambda row: row.update({"support_bundle_path": "local cluster production evidence"}),
            ),
            (
                "unsafe_authority",
                "optimizer_memory_feedback",
                "unsafe_authority",
                lambda row: row.update({"support_bundle_path": "unsafe_authority=true"}),
            ),
            (
                "ceic091_overclaim",
                "agent_evidence_persistence",
                "successor_overclaim",
                lambda row: row.update({"status": "complete_ceic091_integrated_support_bundle"}),
            ),
        ]

        for name, metric_family, expected, change in cases:
            mutated = temp_dir / f"{name}.csv"
            mutate(matrix, mutated, metric_family, change)
            expect_failure_contains(command + ["--matrix", str(mutated)], expected)

    print("ceic_090_metrics_producer_coverage_gate_test=pass")
    print(positive.stdout.strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
