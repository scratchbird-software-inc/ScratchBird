#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Static DBLC-013O protocol/persisted-format versioning gate."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys


REQUIRED_PATTERNS = {
    "project/src/wire/parser_server_ipc/parser_server_ipc.hpp": [
        "kParserServerIpcProtocolCurrent",
        "kParserServerIpcProtocolMinSupported",
        "kParserServerIpcProtocolMaxSupported",
    ],
    "project/src/server/sbps.hpp": [
        "kProtocolMajorMinSupported",
        "kProtocolMajorMaxSupported",
        "kProtocolMinorMinSupported",
        "kProtocolMinorMaxSupported",
    ],
    "project/src/server/server_ipc_lifecycle.hpp": [
        "kServerIpcEndpointDescriptorFormatCurrent",
        "kServerLifecycleStateFileFormatCurrent",
        "current_security_epoch",
        "current_resource_epoch",
    ],
    "project/src/server/config_policy_security_lifecycle.hpp": [
        "kConfigPolicySecurityLifecycleDescriptorCurrent",
    ],
    "project/src/storage/database/startup_state.hpp": [
        "kStartupStateFormatMajorCurrent",
        "kStartupStateFormatMajorMaxSupported",
    ],
    "project/src/storage/disk/database_format.cpp": [
        "kDatabaseFormatMajorCurrent",
        "FORMAT.VERSION_TOO_OLD",
        "FORMAT.VERSION_UNSUPPORTED",
    ],
    "project/src/storage/database/database_lifecycle.cpp": [
        "kDatabaseCatalogManifestFormatCurrent",
        "kResourceSeedManifestFormatCurrent",
        "catalog_manifest_format_version",
        "resource_seed_manifest_format_version",
    ],
    "project/src/parsers/sbsql_worker/common/common.hpp": [
        "kSbsqlWorkerProtocolCurrentVersion",
        "kSbsqlWorkerProtocolMinSupported",
        "kSbsqlWorkerProtocolMaxSupported",
    ],
}


FORBIDDEN_PATTERNS = {
    "project/src/storage/database/database_lifecycle.cpp": [
        "WAL finality",
        "sqlite3",
        "PRAGMA",
    ],
    "project/src/server/ipc_server.cpp": [
        "driver transaction finality",
        "parser transaction finality",
    ],
}


def fail(message: str) -> int:
    print(f"DBLC_STATIC_PROTOCOL_VERSIONING: {message}", file=sys.stderr)
    return 1


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

    for relative, patterns in FORBIDDEN_PATTERNS.items():
        path = repo / relative
        if not path.exists():
            return fail(f"missing forbidden-scan file {relative}")
        text = path.read_text(encoding="utf-8")
        for pattern in patterns:
            if pattern in text:
                return fail(f"{relative} contains forbidden pattern {pattern}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
