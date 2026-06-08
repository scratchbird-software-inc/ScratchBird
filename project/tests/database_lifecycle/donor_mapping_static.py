#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Static gate for DBLC-014 donor lifecycle mapping invariants."""

from __future__ import annotations

import argparse
import pathlib
import re
import sys


OWNED_SOURCE_FILES = (
    "project/src/parsers/sbsql_worker/statement/statement_catalog.hpp",
    "project/src/parsers/sbsql_worker/statement/statement_catalog.cpp",
    "project/src/parsers/sbsql_worker/lowering/lowering.hpp",
    "project/src/parsers/sbsql_worker/lowering/lowering.cpp",
    "project/src/parsers/sbsql_worker/runtime/parser_runtime.hpp",
    "project/src/parsers/sbsql_worker/runtime/parser_runtime.cpp",
    "project/src/parsers/donor/firebird/firebird_dialect.hpp",
    "project/src/parsers/donor/firebird/firebird_dialect.cpp",
    "project/src/parsers/donor/firebird/firebird_worker_session.hpp",
    "project/src/parsers/donor/firebird/firebird_worker_session.cpp",
)

REQUIRED_TOKENS = {
    "project/src/parsers/sbsql_worker/statement/statement_catalog.cpp": (
        "BuiltinSbsqlLifecycleMappings",
        "SBLR_LIFECYCLE_CREATE_DATABASE",
        "SBLR_LIFECYCLE_DROP_DATABASE",
        "SBSQL.EMULATION.NON_FILE_OPERATION",
    ),
    "project/src/parsers/sbsql_worker/statement/statement_catalog.hpp": (
        "produces_file_effects",
        "parser_executes_sql",
    ),
    "project/src/parsers/sbsql_worker/lowering/lowering.cpp": (
        "authority.engine.lifecycle_api_required",
        "authority.engine.mga_lifecycle_evidence_required",
        "sql_text_included",
        "real_file_effects",
        "parser_executes_sql",
    ),
    "project/src/parsers/sbsql_worker/runtime/parser_runtime.cpp": (
        "SbsqlParserLifecycleMappingReportJson",
        "DBLC_P14_DONOR_MAPPING_COMPLETE",
        "DBLC_STATIC_NO_DONOR_ENGINE_SQL",
        "cross_dialect_dependencies",
    ),
    "project/src/parsers/donor/firebird/firebird_dialect.cpp": (
        "FirebirdLifecycleMappings",
        "SBLR_LIFECYCLE_CREATE_DATABASE",
        "EngineCreateLifecycle",
        "FIREBIRD.EMULATION.NON_FILE_SURFACE",
        "real_firebird_file_effects",
        "donor_engine_sql_executed",
    ),
    "project/src/parsers/donor/firebird/firebird_worker_session.cpp": (
        "FirebirdWireLifecycleMappingJson",
        "op_create",
        "op_drop_database",
        "LIFECYCLE_MAPPING",
        "real_firebird_file_effects",
        "donor_engine_sql_executed",
    ),
    "project/tests/database_lifecycle/donor_mapping_conformance.cpp": (
        "DBLC_P14_DONOR_MAPPING_COMPLETE",
        "DBLC-014",
        "donor_engine_sql_executed",
        "parser_executes_sql",
    ),
    "project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/"
    "artifacts/DATABASE_LIFECYCLE_DONOR_MAPPING_REPORT.md": (
        "DBLC_P14_DONOR_MAPPING_COMPLETE",
        "database_lifecycle_donor_mapping",
        "DBLC_STATIC_NO_DONOR_ENGINE_SQL",
    ),
}

FORBIDDEN_PATTERNS = (
    (re.compile(r"\bsqlite3\b", re.IGNORECASE), "SQLite shortcut"),
    (re.compile(r"\bPRAGMA\s+(foreign_keys|journal_mode|synchronous|cache_size|locking_mode|temp_store)\b",
                re.IGNORECASE),
     "PRAGMA shortcut"),
    (re.compile(r"\bjournal_mode\b", re.IGNORECASE), "journal mode shortcut"),
    (re.compile(r"\bWAL\b"), "WAL shortcut"),
    (re.compile(r"execute\s*\([^;\n]*donor[^;\n]*sql", re.IGNORECASE), "donor SQL execution"),
    (re.compile(r"donor_engine_sql_executed\s*=\s*true", re.IGNORECASE),
     "donor engine SQL enabled"),
    (re.compile(r"donor_engine_sql_executed\"?\s*:\s*true", re.IGNORECASE),
     "donor engine SQL enabled"),
    (re.compile(r"parser_executes_sql\"?\s*:\s*true", re.IGNORECASE),
     "parser SQL execution enabled"),
    (re.compile(r"real_firebird_file_effects\"?\s*:\s*true", re.IGNORECASE),
     "Firebird file effects enabled"),
    (re.compile(r"real_file_effects\"?\s*:\s*true", re.IGNORECASE),
     "file effects enabled"),
    (re.compile(r"produces_file_effects[^;\n{}]*true", re.IGNORECASE),
     "mapping produces file effects"),
)


def allowed_forbidden_shortcut(line: str, description: str) -> bool:
    lowered = line.lower()
    if description == "WAL shortcut" and "sb.scalar.refusal_wal" in lowered:
        return True
    return False


def read_file(repo_root: pathlib.Path, rel: str) -> str:
    path = repo_root / rel
    try:
        return path.read_text(encoding="utf-8")
    except FileNotFoundError:
        raise AssertionError(f"missing required DBLC-014 file: {rel}") from None


def assert_required_tokens(repo_root: pathlib.Path) -> None:
    for rel, tokens in REQUIRED_TOKENS.items():
        text = read_file(repo_root, rel)
        for token in tokens:
            if token not in text:
                raise AssertionError(f"{rel}: missing required token {token!r}")


def assert_no_forbidden_shortcuts(repo_root: pathlib.Path) -> None:
    for rel in OWNED_SOURCE_FILES:
        text = read_file(repo_root, rel)
        for pattern, description in FORBIDDEN_PATTERNS:
            for line_no, line_text in enumerate(text.splitlines(), 1):
                match = pattern.search(line_text)
                if match and not allowed_forbidden_shortcut(line_text, description):
                    raise AssertionError(
                        f"{rel}:{line_no}: forbidden {description}: {match.group(0)!r}"
                    )


def assert_independent_dialect_ownership(repo_root: pathlib.Path) -> None:
    sbsql_files = (
        "project/src/parsers/sbsql_worker/statement/statement_catalog.cpp",
        "project/src/parsers/sbsql_worker/lowering/lowering.cpp",
        "project/src/parsers/sbsql_worker/runtime/parser_runtime.cpp",
    )
    firebird_files = (
        "project/src/parsers/donor/firebird/firebird_dialect.cpp",
        "project/src/parsers/donor/firebird/firebird_worker_session.cpp",
    )
    for rel in sbsql_files:
        text = read_file(repo_root, rel)
        if "#include \"firebird" in text or "parser::firebird" in text:
            raise AssertionError(f"{rel}: SBSQL mapping depends on Firebird dialect tree")
    for rel in firebird_files:
        text = read_file(repo_root, rel)
        if "#include \"statement/statement_catalog" in text or "parser::sbsql" in text:
            raise AssertionError(f"{rel}: Firebird mapping depends on SBSQL dialect tree")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", required=True, type=pathlib.Path)
    args = parser.parse_args()
    repo_root = args.repo_root.resolve()

    try:
        assert_required_tokens(repo_root)
        assert_no_forbidden_shortcuts(repo_root)
        assert_independent_dialect_ownership(repo_root)
    except AssertionError as exc:
        print(f"DBLC_STATIC_NO_DONOR_ENGINE_SQL=failed: {exc}", file=sys.stderr)
        return 1

    print("DBLC_STATIC_NO_DONOR_ENGINE_SQL=passed")
    print("DBLC_P14_DONOR_MAPPING_COMPLETE=static-evidence")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
