#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Gate benchmark evidence against embedded ScratchBird runtime storage."""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(message)


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        raise SystemExit("usage: benchmark_runtime_storage_policy_gate.py <repo-root>")

    repo_root = Path(argv[1]).resolve()
    harness_root = repo_root / "docs" / "reference" / "legacy_execution_plan_10_performance_parity" / "benchmark_harness"
    sys.path.insert(0, str(harness_root))
    sys.path.insert(0, str(harness_root / "scripts"))

    from benchmark_provenance import scan_scratchbird_runtime_storage_policy
    from write_comparison_guard import build_guard

    with tempfile.TemporaryDirectory() as tmp:
        runtime_root = Path(tmp) / "scratchbird-runtime"
        runtime_root.mkdir()
        (runtime_root / "main.sbdb").write_bytes(b"scratchbird")
        (runtime_root / "main.sbdb.native-sbwp.sqlite-wal").write_bytes(b"embedded-storage-log")

        failed_policy = scan_scratchbird_runtime_storage_policy(runtime_root)
        require(failed_policy["status"] == "failed", "embedded runtime storage was not rejected")
        require(failed_policy["comparison_eligible"] is False, "failed storage policy remained comparison-eligible")
        require(failed_policy["forbidden_count"] == 1, "forbidden runtime artifact was not counted")

        guard = build_guard(
            {
                "engine": "scratchbird",
                "suite": "all",
                "runtime_options": {
                    "executed_runs": "5",
                    "warmup_runs": "0",
                    "minimum_runs_for_firm_claims": "5",
                    "warm_cold_separation": "cold_and_warm_reported",
                    "summary_statistics": "median",
                    "tail_latency_statistics": "p95_p99",
                    "variance_policy": "sample_stdev_and_cv",
                    "outlier_policy": "report_all_runs",
                    "best_run_policy": "forbidden",
                },
                "scratchbird_runtime": {
                    "pinning": {
                        "comparison_eligible": False,
                        "status": "unpinned",
                        "reason": failed_policy["reason"],
                    },
                    "runtime_storage_policy": failed_policy,
                },
            },
            None,
        )
        require(guard["firm_comparison_eligible"] is False, "comparison guard accepted embedded storage evidence")
        require(
            any("embedded storage/log artifacts" in reason for reason in guard["blocking_reasons"]),
            "comparison guard did not report embedded storage blocker",
        )

        clean_root = Path(tmp) / "clean-scratchbird-runtime"
        clean_root.mkdir()
        (clean_root / "main.sbdb").write_bytes(b"scratchbird")
        passed_policy = scan_scratchbird_runtime_storage_policy(clean_root)
        require(passed_policy["status"] == "passed", "clean runtime storage was rejected")
        require(passed_policy["comparison_eligible"] is True, "clean runtime storage was not comparison-eligible")

    print("benchmark_runtime_storage_policy_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
