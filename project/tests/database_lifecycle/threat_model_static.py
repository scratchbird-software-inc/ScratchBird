#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""DBLC-013T static threat-model and abuse-case gate."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


REQUIRED_TOKENS = {
    "project/src/server/config_policy_security_lifecycle.hpp": [
        "DatabaseLifecycleThreatSurface",
        "EvaluateDatabaseLifecycleThreatModelGate",
        "DBLC_P13T_THREAT_MODEL_COMPLETE",
        "DBLC_STATIC_THREAT_MODEL_ABUSE_CASES",
        "database_lifecycle_threat_model",
    ],
    "project/src/server/config_policy_security_lifecycle.cpp": [
        "DBLC.THREAT.SESSION_REQUIRED",
        "DBLC.THREAT.SECURITY_CONTEXT_REQUIRED",
        "DBLC.THREAT.ENGINE_AUTHORITY_REQUIRED",
        "ENGINE.SHUTDOWN_FORCE_UNSAFE_REFUSED",
        "SB_ENGINE_API_UDR_TRUST_REQUIRED",
        "BACKUP_POLICY_REQUIRED",
        "OPS.HEALTH.REDACTION_REQUIRED",
        "OPS.SUPPORT_BUNDLE.REDACTION_REQUIRED",
        "SERVER.SERVICE_FILE.PRIVATE_MODE_REQUIRED",
        "WORKLOAD_RESOURCE.BYPASS_REFUSED",
    ],
    "project/src/server/manager_control.cpp": [
        "PARSER_SERVER_IPC.SESSION_NOT_BOUND",
        "SECURITY.ACCESS_DENIED",
        "\\\"redaction_state\\\":\\\"redacted",
        "\\\"authorization_authority\\\":\\\"engine",
    ],
    "project/src/server/maintenance_coordinator.cpp": [
        "ENGINE.SHUTDOWN_SCOPE_INVALID",
        "ENGINE.SHUTDOWN_INPUT_INVALID",
        "unknown_transaction_finality_preserved",
        "force_termination_policy_uuid",
        "recovery_evidence_preserved",
    ],
    "project/src/server/ipc_server.cpp": [
        "PARSER_SERVER_IPC.SESSION_BOUND_TOO_EARLY",
        "PARSER_SERVER_IPC.SESSION_NOT_BOUND",
        "chmod(StatePath(config).c_str(), S_IRUSR | S_IWUSR)",
    ],
    "project/src/engine/internal_api/backup_archive/backup_archive_api.cpp": [
        "BACKUP_SECURITY_CONTEXT_REQUIRED",
        "BACKUP_POLICY_REQUIRED",
        "RESTORE_POLICY_REQUIRED",
        "BACKUP_ENGINE_OWNED_PATH_REQUIRED",
        "RESTORE_INSPECTION_OPEN_REQUIRED",
    ],
    "project/src/engine/internal_api/management/support_bundle_api.cpp": [
        "OPS.SUPPORT_BUNDLE.SECURITY_CONTEXT_REQUIRED",
        "OPS.SUPPORT_BUNDLE.ENGINE_AUTHORIZATION_REQUIRED",
        "OPS.SUPPORT_BUNDLE.POLICY_REQUIRED",
        "OPS.SUPPORT_BUNDLE.PROTECTED_MATERIAL_FORBIDDEN",
    ],
    "project/src/core/agents/agent_feature_gates.cpp": [
        "SB_AGENT_CAPABILITY.SECURITY_CONTEXT_REQUIRED",
        "SB_AGENT_CAPABILITY.ENGINE_AUTHORITY_REQUIRED",
        "SB_AGENT_CAPABILITY.AUTHORIZATION_DENIED",
        "SB_AGENT_CAPABILITY.PACKAGE_POLICY_REQUIRED",
        "SB_AGENT_CAPABILITY.PACKAGE_TRUST_REQUIRED",
    ],
    "project/src/engine/internal_api/security/security_model.cpp": [
        "BACKUP_CREATE",
        "BACKUP_RESTORE",
        "BACKUP_CONTROL",
        "SUPPORT_EXPORT",
        "UDR_TRUST_ADMIN",
    ],
    "project/tests/database_lifecycle/threat_model_conformance.cpp": [
        "ApplyDatabaseShutdownOperation",
        "HandleServerManagementRequest",
        "EvaluateBackupArchiveLifecycleAdmission",
        "EnginePrepareSupportBundle",
        "EvaluateFeatureGateRequest",
        "WorkloadResourceQuotaController",
        "DBLC_P13T_THREAT_MODEL_COMPLETE",
        "database_lifecycle_threat_model",
    ],
    "project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/artifacts/DATABASE_LIFECYCLE_THREAT_MODEL_ABUSE_CASES.md": [
        "DBLC_P13T_THREAT_MODEL_COMPLETE",
        "database_lifecycle_threat_model",
        "DBLC_STATIC_THREAT_MODEL_ABUSE_CASES",
        "force shutdown",
        "IPC authentication",
        "IPC authorization",
        "manager supervision",
        "listener supervision",
        "parser supervision",
        "UDR loading",
        "health reporting",
        "backup/restore",
        "support bundles",
        "service files",
        "resource quota",
        "Engine remains the authentication and authorization authority",
        "Parser/listener/driver state is never finality or security authority",
    ],
}


FORBIDDEN_IN_P13T_FILES = [
    "sqlite3",
    "PRAGMA",
    "reference shortcut",
    "parser finality authority",
    "listener finality authority",
    "TODO threat",
    "stub threat",
    "placeholder threat",
]


def read_required(repo_root: Path, relpath: str) -> str:
    path = repo_root / relpath
    if not path.exists():
        raise AssertionError(f"missing required file: {relpath}")
    return path.read_text(encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()

    repo_root = Path(args.repo_root).resolve()
    failures: list[str] = []

    for relpath, tokens in REQUIRED_TOKENS.items():
        try:
            text = read_required(repo_root, relpath)
        except AssertionError as exc:
            failures.append(str(exc))
            continue
        for token in tokens:
            if token not in text:
                failures.append(f"{relpath}: missing token {token!r}")

    p13t_code_paths = [
        "project/src/server/config_policy_security_lifecycle.hpp",
        "project/src/server/config_policy_security_lifecycle.cpp",
        "project/tests/database_lifecycle/threat_model_conformance.cpp",
    ]
    for relpath in p13t_code_paths:
        try:
            text = read_required(repo_root, relpath)
        except AssertionError as exc:
            failures.append(str(exc))
            continue
        for forbidden in FORBIDDEN_IN_P13T_FILES:
            if forbidden in text:
                failures.append(f"{relpath}: forbidden shortcut marker {forbidden!r}")

    if failures:
        for failure in failures:
            print(f"DBLC_STATIC_THREAT_MODEL_ABUSE_CASES failed: {failure}", file=sys.stderr)
        return 1

    print("DBLC_STATIC_THREAT_MODEL_ABUSE_CASES passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
