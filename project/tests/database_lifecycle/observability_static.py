#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""DBLC-015 static diagnostic/message-vector observability audit."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys


OWNED_FILES = [
    "project/src/server/server_observability.hpp",
    "project/src/server/server_observability.cpp",
    "project/src/server/diagnostics.hpp",
    "project/src/server/diagnostics.cpp",
    "project/src/server/manager_control.cpp",
    "project/src/server/maintenance_coordinator.cpp",
    "project/src/engine/internal_api/observability/metrics_api.hpp",
    "project/src/engine/internal_api/observability/metrics_api.cpp",
    "project/src/engine/internal_api/security/audit_api.hpp",
    "project/src/engine/internal_api/security/audit_api.cpp",
    "project/src/engine/internal_api/diagnostics/diagnostic_rendering.hpp",
    "project/src/engine/internal_api/diagnostics/diagnostic_rendering.cpp",
    "project/tests/database_lifecycle/observability_conformance.cpp",
    "project/tests/database_lifecycle/observability_static.py",
    "project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/artifacts/DATABASE_LIFECYCLE_OBSERVABILITY_REPORT.md",
]

REQUIRED_TOKENS = {
    "project/src/server/diagnostics.cpp": [
        "ToPrivateMessageVectorJsonLine",
        "DiagnosticShapeIdForCode",
        "IsRetryableDiagnosticCode",
        "parser_finality_authority",
        "donor_finality_authority",
    ],
    "project/src/server/server_observability.cpp": [
        "RecordServerLifecycleObservability",
        "sys.metrics.lifecycle.operation_total",
        "sys.metrics.lifecycle.cache_invalidation_total",
        "ServerCacheInvalidationMarker",
        "CanonicalLifecycleObservabilityOperations",
    ],
    "project/src/server/manager_control.cpp": [
        "RecordManagementLifecycleObservability",
        "IsLifecycleObservabilityOperation",
        "FirstDiagnosticCode",
        "engine authorization denied management route",
    ],
    "project/src/engine/internal_api/observability/metrics_api.cpp": [
        "EngineRecordLifecycleMetric",
        "sb_lifecycle_operation_total",
        "sb_lifecycle_cache_invalidation_total",
        "parser_finality_authority",
    ],
    "project/src/engine/internal_api/security/audit_api.cpp": [
        "EngineEmitLifecycleAuditEvent",
        "public_private_shape_separated",
        "lifecycle_cache_invalidation",
    ],
    "project/src/engine/internal_api/diagnostics/diagnostic_rendering.cpp": [
        "parser_finality_authority_must_be_false",
        "donor_finality_authority_must_be_false",
        "diagnostic_public_shape_required",
        "diagnostic_private_shape_required",
    ],
    "project/tests/database_lifecycle/observability_conformance.cpp": [
        "TestDiagnosticShapes",
        "TestServerLifecycleObservability",
        "TestEngineMetricsAndAudit",
        "TestParserRendering",
    ],
}

FORBIDDEN = [
    "sqlite3",
    "SQL PRAGMA",
    "journal_mode",
    "WAL recovery",
    "TODO DBLC-015",
    "placeholder observability",
    "deferred observability claim",
]


def read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except FileNotFoundError:
        raise AssertionError(f"missing required owned file: {path}") from None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()
    root = Path(args.repo_root).resolve()

    failures: list[str] = []
    combined = ""
    forbidden_scan = ""
    for rel in OWNED_FILES:
        path = root / rel
        try:
            text = read(path)
        except AssertionError as exc:
            failures.append(str(exc))
            continue
        combined += f"\n--- {rel} ---\n{text}"
        if rel != "project/tests/database_lifecycle/observability_static.py":
            forbidden_scan += f"\n--- {rel} ---\n{text}"
        for token in REQUIRED_TOKENS.get(rel, []):
            if token not in text:
                failures.append(f"{rel}: missing required token `{token}`")

    lower_combined = forbidden_scan.lower()
    for token in FORBIDDEN:
        if token.lower() in lower_combined:
            failures.append(f"forbidden observability drift token present: `{token}`")

    required_operations = [
        "create_database",
        "open_database",
        "attach_database",
        "detach_database",
        "begin_transaction",
        "commit_transaction",
        "rollback_transaction",
        "enter_database_maintenance",
        "verify_database",
        "repair_database",
        "shutdown_database",
        "force_shutdown_database",
        "drop_database",
        "parser_package_route",
        "upgrade_database",
    ]
    for op in required_operations:
        if op not in combined:
            failures.append(f"canonical lifecycle operation missing from observability coverage: {op}")

    if "DBLC_P15_OBSERVABILITY_COMPLETE" not in combined:
        failures.append("missing DBLC_P15_OBSERVABILITY_COMPLETE evidence marker")
    if "database_lifecycle_supportability_evidence" not in combined:
        failures.append("missing database_lifecycle_supportability_evidence gate marker")
    if "DBLC_STATIC_DIAGNOSTIC_MESSAGE_VECTOR_AUDIT" not in combined:
        failures.append("missing DBLC_STATIC_DIAGNOSTIC_MESSAGE_VECTOR_AUDIT gate marker")

    if failures:
        for failure in failures:
            print(f"DBLC-015 static audit failure: {failure}", file=sys.stderr)
        return 1
    print("DBLC_STATIC_DIAGNOSTIC_MESSAGE_VECTOR_AUDIT passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
