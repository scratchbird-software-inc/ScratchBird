#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""FSPE-014F benchmark baseline publication gate."""

from __future__ import annotations

import argparse
from pathlib import Path


ARTIFACT_ROOT = Path("project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/artifacts")
REPORT = ARTIFACT_ROOT / "BENCHMARK_BASELINE_REPORT.md"

REQUIRED_TOKENS = (
    "Status: complete",
    "non-tuning",
    "Machine And Build Profile",
    "Repo build id",
    "SCRATCHBIRD_ENABLE_DEBUG_LOGS=OFF",
    "SCRATCHBIRD_ENABLE_HOTPATH_TRACE=OFF",
    "SCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE=OFF",
    "SCRATCHBIRD_ENABLE_PREPARED_TRACE=OFF",
    "Fixture Labels And Route Anchors",
    "sbp_sbsql_lexer_conformance_probe",
    "sbp_sbsql_binder_authority_conformance_probe",
    "sbp_sbsql_sblr_lowering_verifier_conformance_probe",
    "sbu_sbsql_parser_support_probe",
    "sb_server_sbsql_admission_conformance",
    "sb_listener_sbp_sbsql_server_engine_execution_smoke",
    "sbsql_parser_worker",
    "Command Lines",
    "/usr/bin/time -f",
    "Retained Raw Summaries",
    "elapsed=0.37 maxrss_kb=26020",
    "elapsed=1.97 maxrss_kb=49344",
    "100% tests passed, 0 tests failed out of 39",
    "not performance claims",
)

FORBIDDEN_TOKENS = (
    "Status: planned",
    "TBD",
    "TODO",
    "placeholder",
    "competitive benchmark",
)


class Gate:
    def __init__(self) -> None:
        self.failures: list[str] = []

    def require(self, condition: bool, message: str) -> None:
        if not condition:
            self.failures.append(message)

    def finish(self) -> int:
        if self.failures:
            print("sbsql_benchmark_baseline_gate: failed")
            for failure in self.failures:
                print(f" - {failure}")
            return 1
        print("sbsql_benchmark_baseline_gate: passed")
        return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path.cwd())
    args = parser.parse_args()

    repo = args.repo_root.resolve()
    report = repo / REPORT
    gate = Gate()
    gate.require(report.exists(), "benchmark baseline report is missing")
    text = report.read_text(encoding="utf-8", errors="replace") if report.exists() else ""

    for token in REQUIRED_TOKENS:
        gate.require(token in text, f"benchmark baseline report missing token: {token}")
    for token in FORBIDDEN_TOKENS:
        gate.require(token not in text, f"benchmark baseline report contains forbidden token: {token}")

    return gate.finish()


if __name__ == "__main__":
    raise SystemExit(main())
