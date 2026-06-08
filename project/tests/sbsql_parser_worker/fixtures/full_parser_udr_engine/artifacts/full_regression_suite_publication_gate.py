#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""FSPE-014C full regression suite publication gate."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys


ARTIFACT_ROOT = Path("project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/artifacts")
REPORT = ARTIFACT_ROOT / "FULL_REGRESSION_SUITE_PUBLICATION.md"

REQUIRED_TOKENS = (
    "Status: complete",
    "cmake -S project -B build/sbsql_parser_worker_validation",
    "ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_parser_worker --output-on-failure",
    "sbsql_upgrade_migration_compatibility_gate",
    "sbsql_security_redaction_side_channel_gate",
    "sbsql_fixed_uuid_catalog_seed_gate",
    "sbsql_deterministic_no_network_gate",
    "sbsql_exhaustive_e2e_regression",
    "project/tests/sbsql_parser_worker/generated/replay/",
    "project/tests/sbsql_parser_worker/generated/exhaustive_e2e/",
    "project/tests/sbsql_parser_worker/generated/upgrade/",
    "Runtime And Disk Expectations",
    "Failure Triage",
    "Cleanup Commands",
    "Retain `project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/artifacts/`",
)


class Gate:
    def __init__(self) -> None:
        self.failures: list[str] = []

    def require(self, condition: bool, message: str) -> None:
        if not condition:
            self.failures.append(message)

    def finish(self) -> int:
        if self.failures:
            print("sbsql_full_regression_suite_publication_gate: failed")
            for failure in self.failures:
                print(f" - {failure}")
            return 1
        print("sbsql_full_regression_suite_publication_gate: passed")
        return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path.cwd())
    args = parser.parse_args()
    repo = args.repo_root.resolve()
    gate = Gate()
    report = repo / REPORT
    gate.require(report.exists(), "full regression publication report is missing")
    text = report.read_text(encoding="utf-8", errors="replace") if report.exists() else ""
    for token in REQUIRED_TOKENS:
        gate.require(token in text, f"publication report missing token: {token}")
    gate.require("This artifact will be populated" not in text, "publication report still contains placeholder text")
    return gate.finish()


if __name__ == "__main__":
    raise SystemExit(main())
