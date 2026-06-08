#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""FSPE-014E cleanup and artifact retention gate."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys


ARTIFACT_ROOT = Path("project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/artifacts")
POLICY = ARTIFACT_ROOT / "CLEANUP_ARTIFACT_RETENTION_POLICY.md"
REQUIRED_TOKENS = (
    "Status: complete",
    "Final-Run Retention Evidence",
    "Final Cleanup Commands",
    "build/sbsql_parser_worker_validation/",
    "project/tests/sbsql_parser_worker/generated/",
    "DETERMINISTIC_ARTIFACT_MANIFEST.csv",
    "/tmp/sb_*.log",
    "/tmp/sb_*.sock",
    "/tmp/sb_*.sbdb",
    "__pycache__",
    "Do not delete canonical specs",
    "FSPE-014E Closure",
)


class Gate:
    def __init__(self) -> None:
        self.failures: list[str] = []

    def require(self, condition: bool, message: str) -> None:
        if not condition:
            self.failures.append(message)

    def finish(self) -> int:
        if self.failures:
            print("sbsql_cleanup_artifact_retention_gate: failed")
            for failure in self.failures:
                print(f" - {failure}")
            return 1
        print("sbsql_cleanup_artifact_retention_gate: passed")
        return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path.cwd())
    args = parser.parse_args()
    repo = args.repo_root.resolve()
    gate = Gate()
    policy = repo / POLICY
    gate.require(policy.exists(), "cleanup policy is missing")
    text = policy.read_text(encoding="utf-8", errors="replace") if policy.exists() else ""
    for token in REQUIRED_TOKENS:
        gate.require(token in text, f"cleanup policy missing token: {token}")
    return gate.finish()


if __name__ == "__main__":
    raise SystemExit(main())
