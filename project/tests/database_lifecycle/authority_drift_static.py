#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""DBLC-017 authority drift gate.

The gate is intentionally scoped to accepted lifecycle, parser-admission,
reference-mapping, and MGA authority paths. It allows explicit anti-WAL/refusal
evidence, but rejects any shortcut that would make WAL, SQLite/PRAGMA,
parser finality, reference SQL, or cluster paths authoritative.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys


SCAN_FILES = (
    "project/src/storage/database/database_lifecycle.cpp",
    "project/src/storage/database/database_dirty_manifest.cpp",
    "project/src/storage/database/startup_state.cpp",
    "project/src/server/sblr_admission.cpp",
    "project/src/server/sblr_dispatch_server.cpp",
    "project/src/server/session_registry.cpp",
    "project/src/server/manager_control.cpp",
    "project/src/parsers/compatibility/firebird/firebird_dialect.cpp",
    "project/src/parsers/compatibility/firebird/firebird_worker_session.cpp",
    "project/src/parsers/sbsql_worker/lowering/lowering.cpp",
    "project/src/parsers/sbsql_worker/runtime/parser_runtime.cpp",
    "project/src/transaction/mga/cluster_transaction_fail_closed.cpp",
    "project/src/transaction/mga/transaction_evidence.cpp",
    "project/src/engine/internal_api/cluster/cluster_insert_route_api.cpp",
    "project/src/engine/internal_api/cluster/remote_participant_insert_api.cpp",
    "project/src/engine/internal_api/backup_archive/backup_archive_api.cpp",
)

REQUIRED_TOKENS = {
    "project/src/storage/database/database_dirty_manifest.cpp": (
        "RECOVERY.MANIFEST_WAL_CONFUSION_FORBIDDEN",
        "dirty manifest must not contain WAL",
    ),
    "project/src/server/sblr_admission.cpp": (
        "SBLR.SQL_TEXT_FORBIDDEN",
        "raw_sql_forbidden",
        "cluster_mapping_unavailable",
    ),
    "project/src/server/sblr_dispatch_server.cpp": (
        "requires_public_abi_dispatch",
        "ValidateTransactionAdmission",
        "DispatchThroughPublicAbi",
    ),
    "project/src/server/session_registry.cpp": (
        "SECURITY.AUTHENTICATION.FAILED",
        "ENGINE.DBLC_ATTACH_ADMISSION_DENIED",
    ),
    "project/src/parsers/compatibility/firebird/firebird_dialect.cpp": (
        "\\\"reference_engine_sql_executed\\\":false",
        "\\\"real_firebird_file_effects\\\":false",
        "\\\"sql_text_included\\\":false",
    ),
    "project/src/transaction/mga/cluster_transaction_fail_closed.cpp": (
        "SB-CLUSTER-MAPPING-UNAVAILABLE",
        "cluster_transaction_fail_closed",
    ),
    "project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/"
    "artifacts/DATABASE_LIFECYCLE_HARDENING_REPORT.md": (
        "DBLC_P17_HARDENED",
        "database_lifecycle_fault_injection",
        "DBLC_STATIC_AUTHORITY_DRIFT_GATES",
    ),
}

FORBIDDEN_PATTERNS = (
    (re.compile(r"#\s*include\s*[<\"]sqlite3\.h[>\"]", re.IGNORECASE), "SQLite include"),
    (re.compile(r"\bsqlite3_[A-Za-z0-9_]+\b"), "SQLite API"),
    (re.compile(r"\bExecSqlite\b"), "SQLite execution shortcut"),
    (re.compile(r"\bPRAGMA\s+(foreign_keys|journal_mode|synchronous)\b", re.IGNORECASE),
     "PRAGMA authority shortcut"),
    (re.compile(r"\bjournal_mode\s*=\s*WAL\b", re.IGNORECASE), "WAL journal mode"),
    (re.compile(r"\bwal_checkpoint\b", re.IGNORECASE), "WAL checkpoint"),
    (re.compile(r"\bauthoritative\s+WAL\b", re.IGNORECASE), "authoritative WAL"),
    (re.compile(r"\bWAL\s+recovery\b", re.IGNORECASE), "WAL recovery"),
    (re.compile(r"\brecovery\b.*\bWAL\b", re.IGNORECASE), "WAL recovery"),
    (re.compile(r"\bwrite[- ]ahead\b.*\bauthority\b", re.IGNORECASE),
     "write-ahead authority"),
    (re.compile(r"\bredo\b.*\bauthority\b", re.IGNORECASE), "redo authority"),
    (re.compile(r"\bundo\b.*\bauthority\b", re.IGNORECASE), "undo authority"),
    (re.compile(r"\bparser[- ]managed\s+transaction\s+authority\b", re.IGNORECASE),
     "parser transaction authority"),
    (re.compile(r"\bparser\s+owns\s+transaction\s+finality\b", re.IGNORECASE),
     "parser finality authority"),
    (re.compile(r"\bparser.*\bstorage\s+authority\b", re.IGNORECASE),
     "parser storage authority"),
    (re.compile(r"\breference_engine_sql_executed\s*=\s*true", re.IGNORECASE),
     "reference SQL execution enabled"),
    (re.compile(r"\"reference_engine_sql_executed\"\s*:\s*true", re.IGNORECASE),
     "reference SQL execution enabled"),
    (re.compile(r"\"real_firebird_file_effects\"\s*:\s*true", re.IGNORECASE),
     "reference file effect enabled"),
    (re.compile(r"\"parser_executes_sql\"\s*:\s*true", re.IGNORECASE),
     "parser SQL execution enabled"),
)

NEGATIVE_AUTHORITY_CONTEXT = (
    "must not",
    "never",
    "not authority",
    "not authoritative",
    "not recovery authority",
    "not durable authority",
    "no wal",
    "anti-wal",
    "without wal",
    "does not contain wal",
    "forbidden",
    "confusion_forbidden",
    "authoritative_wal_forbidden",
    "wal is not",
)


def read(repo_root: Path, rel: str) -> str:
    path = repo_root / rel
    try:
        return path.read_text(encoding="utf-8")
    except FileNotFoundError:
        raise AssertionError(f"missing required DBLC-017 file: {rel}") from None


def negative_context(line: str) -> bool:
    lowered = line.lower()
    return any(token in lowered for token in NEGATIVE_AUTHORITY_CONTEXT)


def assert_required_tokens(repo_root: Path) -> None:
    for rel, tokens in REQUIRED_TOKENS.items():
        text = read(repo_root, rel)
        for token in tokens:
            if token not in text:
                raise AssertionError(f"{rel}: missing required authority token {token!r}")


def assert_no_authority_drift(repo_root: Path) -> None:
    for rel in SCAN_FILES:
        text = read(repo_root, rel)
        for line_no, line in enumerate(text.splitlines(), 1):
            for pattern, description in FORBIDDEN_PATTERNS:
                match = pattern.search(line)
                if not match:
                    continue
                if negative_context(line):
                    continue
                if rel.endswith("database_dirty_manifest.cpp") and "WAL" in line:
                    continue
                raise AssertionError(
                    f"{rel}:{line_no}: forbidden {description}: {match.group(0)!r}"
                )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", required=True, type=Path)
    args = parser.parse_args()
    repo_root = args.repo_root.resolve()

    try:
        assert_required_tokens(repo_root)
        assert_no_authority_drift(repo_root)
    except AssertionError as exc:
        print(f"DBLC_STATIC_AUTHORITY_DRIFT_GATES=failed: {exc}", file=sys.stderr)
        return 1

    print("DBLC_STATIC_AUTHORITY_DRIFT_GATES=passed")
    print("DBLC_P17_HARDENED=static-authority-evidence")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
