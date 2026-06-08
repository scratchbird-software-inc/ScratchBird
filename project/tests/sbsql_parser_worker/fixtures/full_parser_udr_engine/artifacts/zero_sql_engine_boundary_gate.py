#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""FSPE-014B zero-SQL engine-boundary gate."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys


REQUIRED_FILES = {
    "server_admission": Path("project/src/server/sblr_admission.cpp"),
    "engine_api": Path("project/src/engine/internal_api/engine_internal_api.cpp"),
    "engine_envelope": Path("project/src/engine/sblr/sblr_engine_envelope.cpp"),
    "parser_support_udr": Path("project/src/udr/sbu_sbsql_parser_support/sbu_sbsql_parser_support.cpp"),
    "boundary_spec": Path("public_contract_snapshot"),
    "hardening_gate": Path("project/tests/sbsql_parser_worker/generated/hardening/sbsql_no_spin_no_wal_no_direct_db_gate.cpp"),
    "upgrade_gate": Path("project/tests/sbsql_parser_worker/generated/upgrade/sbsql_upgrade_migration_compatibility_gate.cpp"),
}

FORBIDDEN_ENGINE_EXECUTION_PATTERNS = (
    r"\bExecuteSql\b",
    r"\bexecute_sql\s*\(",
    r"\bsqlite3_exec\s*\(",
    r"\bPQexec\s*\(",
    r"\bmysql_query\s*\(",
    r"\bSQLExecDirect\s*\(",
    r"\bSQLPrepare\s*\(",
    r"\bisc_dsql_execute_immediate\s*\(",
)


class Gate:
    def __init__(self) -> None:
        self.failures: list[str] = []

    def require(self, condition: bool, message: str) -> None:
        if not condition:
            self.failures.append(message)

    def finish(self) -> int:
        if self.failures:
            print("sbsql_zero_sql_engine_boundary_gate: failed")
            for failure in self.failures:
                print(f" - {failure}")
            return 1
        print("sbsql_zero_sql_engine_boundary_gate: passed")
        return 0


def text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def scan_engine_for_forbidden_execution(repo: Path, gate: Gate) -> None:
    for path in (repo / "project/src/engine").rglob("*"):
        if not path.is_file() or path.suffix not in {".cpp", ".hpp", ".h", ".c"}:
            continue
        content = text(path)
        for pattern in FORBIDDEN_ENGINE_EXECUTION_PATTERNS:
            if re.search(pattern, content):
                gate.require(False, f"forbidden SQL execution token {pattern} found in {path.relative_to(repo)}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path.cwd())
    args = parser.parse_args()
    repo = args.repo_root.resolve()
    gate = Gate()

    loaded: dict[str, str] = {}
    for name, relative in REQUIRED_FILES.items():
        path = repo / relative
        gate.require(path.exists(), f"required boundary evidence file missing: {relative}")
        if path.exists():
            loaded[name] = text(path)

    server = loaded.get("server_admission", "")
    for token in [
        "ContainsSqlTextMarker",
        "SBLR.SQL_TEXT_FORBIDDEN",
        "raw_sql_forbidden",
        "parser_resolved_names_to_uuids",
        "SBLRExecutionEnvelope.v3",
    ]:
        gate.require(token in server, f"server admission missing boundary token {token}")

    engine_api = loaded.get("engine_api", "")
    for token in [
        "envelope.contains_sql_text",
        "SB-ENGINE-API-SQL-TEXT-NOT-ACCEPTED",
        "engine.api.sql_text_not_accepted",
        "parser_resolved_names_to_uuids",
        "SB-ENGINE-API-PARSER-MUST-NOT-BE-TRUSTED",
    ]:
        gate.require(token in engine_api, f"engine API missing boundary token {token}")

    engine_envelope = loaded.get("engine_envelope", "")
    for token in [
        "contains_sql_text",
        "SB_SBLR_SQL_TEXT_FORBIDDEN",
        "parser_resolved_names_to_uuids",
    ]:
        gate.require(token in engine_envelope, f"engine SBLR envelope missing boundary token {token}")

    udr = loaded.get("parser_support_udr", "")
    for token in ["ParseBindLower", "LowerToSblr", "VerifySblrEnvelope", "engine_context=trusted"]:
        gate.require(token in udr, f"parser-support UDR missing token {token}")
    for forbidden in ["DispatchSblrOperation", "OpenDatabaseFile", "CreateDatabaseFile"]:
        gate.require(forbidden not in udr, f"parser-support UDR must not call {forbidden}")

    boundary_spec = loaded.get("boundary_spec", "")
    for token in [
        "Engine execution code must not execute SQL text",
        "Engine work references UUIDs/descriptors",
        "project/src/server/sblr_admission.cpp",
        "ZERO_SQL_ENGINE_BOUNDARY_AUDIT.md",
    ]:
        gate.require(token in boundary_spec, f"boundary spec missing token {token}")

    hardening = loaded.get("hardening_gate", "")
    upgrade = loaded.get("upgrade_gate", "")
    gate.require("no_wal_recovery=true" in hardening, "hardening gate missing no-WAL evidence")
    gate.require("SBLR.SQL_TEXT_FORBIDDEN" in upgrade, "upgrade gate missing SQL-text refusal evidence")

    scan_engine_for_forbidden_execution(repo, gate)
    return gate.finish()


if __name__ == "__main__":
    raise SystemExit(main())
