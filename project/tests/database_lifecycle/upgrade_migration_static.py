#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Static DBLC-013S migration/no-guessing gate."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys


REQUIRED_PATTERNS = {
    "project/src/storage/database/database_lifecycle.hpp": [
        "DatabaseArtifactVersionCompatibilityRequest",
        "migration_required_without_plan_refused",
        "ClassifyDatabaseCatalogMigrationEvidence",
    ],
    "project/src/storage/database/database_lifecycle.cpp": [
        "ENGINE.DBLC_MIGRATION_AMBIGUOUS_IDENTITY_REFUSED",
        "ENGINE.DBLC_MIGRATION_REQUIRED_WITHOUT_PLAN",
        "ENGINE.DBLC_MIGRATION_PLAN_MISSING",
        "ENGINE.DBLC_FORMAT_DOWNGRADE_REFUSED",
        "ENGINE.DBLC_FORMAT_NEWER_THAN_SUPPORTED",
        "ValidateCatalogMigrationEvidence",
    ],
    "project/src/storage/database/startup_state.hpp": [
        "StartupStateFormatCompatibilityClass",
        "ClassifyStartupStateFormatCompatibility",
    ],
    "project/src/storage/database/startup_state.cpp": [
        "SB-STARTUP-STATE-MIGRATION-REQUIRED-WITHOUT-PLAN",
        "SB-STARTUP-STATE-MIGRATION-PLAN-MISSING",
    ],
    "project/src/server/config.hpp": [
        "ServerConfigCompatibilityClass",
        "ClassifyServerConfigFormat",
    ],
    "project/src/server/config.cpp": [
        "CONFIG.MIGRATION_REQUIRED_WITHOUT_PLAN",
        "CONFIG.MIGRATION_PLAN_MISSING",
        "CONFIG.VERSION_NEWER_THAN_SUPPORTED",
    ],
    "project/src/server/server_ipc_lifecycle.hpp": [
        "ServerLifecycleArtifactMigrationRequest",
        "EvaluateServerLifecycleArtifactMigration",
    ],
    "project/src/server/server_ipc_lifecycle.cpp": [
        "IPC.LIFECYCLE.MIGRATION_REQUIRED_WITHOUT_PLAN",
        "IPC.LIFECYCLE.AMBIGUOUS_IDENTITY_REFUSED",
        "IPC.LIFECYCLE.VERSION_NEWER_THAN_SUPPORTED",
    ],
    "project/tests/database_lifecycle/upgrade_migration_conformance.cpp": [
        "ENGINE.DBLC_MIGRATION_AMBIGUOUS_IDENTITY_REFUSED",
        "CONFIG.MIGRATION_REQUIRED_WITHOUT_PLAN",
        "IPC.LIFECYCLE.MIGRATION_REQUIRED_WITHOUT_PLAN",
    ],
}


FORBIDDEN_LINE_PATTERNS = [
    re.compile(r"fallback\s+to\s+uuid\s+order", re.IGNORECASE),
    re.compile(r"uuid\s+order(?:ing)?\s+as\s+(?:identity|finality|migration)", re.IGNORECASE),
    re.compile(r"timestamp\s+as\s+(?:identity|finality|migration)", re.IGNORECASE),
    re.compile(r"guess(?:es|ing)?\s+(?:database\s+)?identity", re.IGNORECASE),
    re.compile(r"guess(?:es|ing)?\s+(?:format|policy|transaction\s+outcome)", re.IGNORECASE),
    re.compile(r"\bsqlite3\b", re.IGNORECASE),
    re.compile(r"\bPRAGMA\b"),
]


SCAN_FILES = [
    "project/src/storage/database/database_lifecycle.hpp",
    "project/src/storage/database/database_lifecycle.cpp",
    "project/src/storage/database/startup_state.hpp",
    "project/src/storage/database/startup_state.cpp",
    "project/src/storage/disk/database_format.cpp",
    "project/src/server/config.hpp",
    "project/src/server/config.cpp",
    "project/src/server/server_ipc_lifecycle.hpp",
    "project/src/server/server_ipc_lifecycle.cpp",
    "project/tests/database_lifecycle/upgrade_migration_conformance.cpp",
]


def fail(message: str) -> int:
    print(f"DBLC_STATIC_MIGRATION_NO_GUESSING: {message}", file=sys.stderr)
    return 1


def line_allowed(line: str) -> bool:
    return "guess_identity=false" in line


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()
    repo = Path(args.repo_root)

    for relative, patterns in REQUIRED_PATTERNS.items():
        path = repo / relative
        if not path.exists():
            return fail(f"missing required file {relative}")
        text = path.read_text(encoding="utf-8")
        for pattern in patterns:
            if pattern not in text:
                return fail(f"{relative} missing {pattern}")

    for relative in SCAN_FILES:
        path = repo / relative
        if not path.exists():
            return fail(f"missing scan file {relative}")
        for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if line_allowed(line):
                continue
            for pattern in FORBIDDEN_LINE_PATTERNS:
                if pattern.search(line):
                    return fail(f"{relative}:{line_number} contains forbidden no-guessing pattern")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
