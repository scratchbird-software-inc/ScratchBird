#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""FSPE-014D known-risk burn-down gate."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys


ARTIFACT_ROOT = Path("project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/artifacts")
REPORT = ARTIFACT_ROOT / "KNOWN_RISK_BURN_DOWN_REPORT.md"

REQUIRED_TOKENS = (
    "Status: complete",
    "Corrected Issues",
    "Remaining Non-Blocking Risks",
    "No accepted SBSQL surface is hidden behind a residual risk.",
    "Zero open non-cluster engine gaps are masked by this report.",
    "No parser authority leak is accepted as a known risk.",
    "No missing message-vector path is accepted as a known risk.",
    "No WAL recovery or SQL-in-engine authority is accepted as a known risk.",
    "Global donor exact-extraction manifest drift",
    "Broad default `cmake --build ...`",
    "function_seed_registry.cpp",
)


class Gate:
    def __init__(self) -> None:
        self.failures: list[str] = []

    def require(self, condition: bool, message: str) -> None:
        if not condition:
            self.failures.append(message)

    def finish(self) -> int:
        if self.failures:
            print("sbsql_known_risk_burn_down_gate: failed")
            for failure in self.failures:
                print(f" - {failure}")
            return 1
        print("sbsql_known_risk_burn_down_gate: passed")
        return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path.cwd())
    args = parser.parse_args()
    repo = args.repo_root.resolve()
    gate = Gate()
    report = repo / REPORT
    gate.require(report.exists(), "known-risk burn-down report is missing")
    text = report.read_text(encoding="utf-8", errors="replace") if report.exists() else ""
    for token in REQUIRED_TOKENS:
        gate.require(token in text, f"known-risk report missing token: {token}")
    gate.require("This artifact will be populated" not in text, "known-risk report still contains placeholder text")
    return gate.finish()


if __name__ == "__main__":
    raise SystemExit(main())
