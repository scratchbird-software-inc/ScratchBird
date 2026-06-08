#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Static DBLC-013Q packaging/service runtime cleanup gate."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys


GATE = "DBLC_P13Q_PACKAGING_SERVICE_COMPLETE"
LABELS = (
    "database_lifecycle_packaging_service",
    "DBLC_STATIC_RUNTIME_CLEANUP",
)

REQUIRED_PATTERNS = {
    "project/src/server/config.hpp": [
        "database_runtime_scope_id",
        "ServerDatabaseRuntimeScopeId",
    ],
    "project/src/server/lifecycle.hpp": [
        "ServerRuntimeArtifactValidation",
        "ServerRuntimeCleanupOperation",
        "ValidateServerRuntimeArtifacts",
        "CleanupServerRuntimeArtifacts",
    ],
    "project/src/server/lifecycle.cpp": [
        "SERVER.RUNTIME.OWNER_TOKEN_CROSS_DATABASE",
        "SERVER.RUNTIME.DATABASE_SCOPE_INVALID",
        "SERVER.RUNTIME.ENDPOINT_DESCRIPTOR_INVALID",
        "database_runtime_scope_id=",
        "database_path=",
        "ApplyPrivatePermissions",
        "HasPrivatePermissions",
    ],
    "project/src/server/server_daemon_lifecycle.hpp": [
        "runtime_directories_valid",
        "pid_owner_state_valid",
        "endpoint_descriptors_valid",
        "database_association_valid",
        "standalone_cluster_path_refused",
    ],
    "project/src/server/server_daemon_lifecycle.cpp": [
        "ValidateServerRuntimeArtifacts",
        "SERVER.DAEMON.DATABASE_SCOPE_INVALID",
        "SERVER.DAEMON.CLUSTER_AUTHORITY_REQUIRED",
    ],
    "project/src/manager/node/manager_runtime.hpp": [
        "owner_database_runtime_scope_id",
        "ManagerRuntimeValidation",
        "ManagerRuntimeCleanupOperation",
        "ValidateManagerRuntimeArtifacts",
        "CleanupManagerRuntimeArtifacts",
    ],
    "project/src/manager/node/manager_runtime.cpp": [
        "MANAGER.OWNER_DATABASE_SCOPE_INVALID",
        "MANAGER.SERVICE_MODE_UNSUPPORTED",
        "Windows service-control handoff is not implemented in this build.",
        "DaemonizeService",
        "owner_database_runtime_scope_id=",
        "ResolveManagerRuntimePaths",
        "ValidateManagerRuntimeArtifacts",
        "CleanupManagerRuntimeArtifacts",
    ],
    "project/tests/database_lifecycle/packaging_service_conformance.cpp": [
        GATE,
        LABELS[0],
        LABELS[1],
        "TestCrossDatabaseOwnerTokenFailsClosed",
        "TestCleanupPreservesUnrelatedRuntimeArtifacts",
        "TestManagerRuntimeOwnerScopeAndCleanup",
    ],
    "project/tests/manager/runtime_integration_tests.cpp": [
        "TestServiceValidateConfigHasNoDaemonSideEffects",
        "config.service = true",
        "config.validate_config = true",
        "MANAGER.SERVICE_MODE_UNSUPPORTED",
        "state=stopped",
    ],
}

FORBIDDEN_PATTERNS = {
    "project/src/server/lifecycle.cpp": [
        "sqlite3",
        "PRAGMA",
        "WAL finality",
        "std::filesystem::remove_all(config.control_dir",
        "std::filesystem::remove_all(config.data_dir",
    ],
    "project/src/manager/node/manager_runtime.cpp": [
        "sqlite3",
        "PRAGMA",
        "WAL finality",
        "std::filesystem::remove_all(config.control_dir",
        "std::filesystem::remove_all(config_.control_dir",
    ],
}


def fail(message: str) -> int:
    print(f"DBLC_STATIC_RUNTIME_CLEANUP: {message}", file=sys.stderr)
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
