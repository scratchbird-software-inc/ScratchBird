#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Gate for donor low-level utility unsupported-denied policy coverage."""

from __future__ import annotations

import argparse
import csv
import json
import pathlib
import sys


SPEC_REL = (
    "project/tests/donor_sql_parser_first_tranche/"
    "LOW_LEVEL_UTILITY_UNSUPPORTED_POLICY.md"
)
MATRIX_REL = (
    "project/tests/donor_sql_parser_first_tranche/"
    "low_level_utility_unsupported_policy_matrix.csv"
)

REQUIRED_COLUMNS = {
    "donor_id",
    "display_name",
    "parser_package",
    "denied_surfaces",
    "required_message_vector_code",
    "donor_parser_policy",
    "sbsql_authority",
    "parser_runtime_requirement",
    "proof_test",
    "notes",
}

REQUIRED_PHRASES = (
    "repair",
    "verification",
    "checkpoint",
    "SBsql-only",
    "unsupported-denied",
    "<DONOR>.AUTHORITY.UNSUPPORTED_DENIED",
    "no parser-support UDR route",
    "no ScratchBird lifecycle repair/verify API",
    "MGA transaction and recovery authority remain engine-owned",
    "DONOR_LOW_LEVEL_UTILITY_UNSUPPORTED_DENIED_POLICY",
)

EXPECTED_UNSUPPORTED_DENIALS = {
    "cassandra": "CASSANDRA.AUTHORITY.UNSUPPORTED_DENIED",
    "clickhouse": "CLICKHOUSE.AUTHORITY.UNSUPPORTED_DENIED",
    "duckdb": "DUCKDB.AUTHORITY.UNSUPPORTED_DENIED",
    "firebird": "FIREBIRD.AUTHORITY.UNSUPPORTED_DENIED",
    "mariadb": "MARIADB.AUTHORITY.UNSUPPORTED_DENIED",
    "mysql": "MYSQL.AUTHORITY.UNSUPPORTED_DENIED",
    "postgresql": "POSTGRESQL.AUTHORITY.UNSUPPORTED_DENIED",
    "sqlite": "SQLITE.AUTHORITY.UNSUPPORTED_DENIED",
}

FORBIDDEN_DONOR_SOURCE_SNIPPETS = (
    "ParserSupportMaintenanceRoute",
    "SBLR_DONOR_MYSQL_MAINTENANCE_ROUTE",
    "SBLR_DONOR_MARIADB_MAINTENANCE_ROUTE",
    "SBLR_DONOR_POSTGRESQL_MAINTENANCE_ROUTE",
    "SBLR_DONOR_SQLITE_MAINTENANCE_ROUTE",
    "SBLR_DONOR_CLICKHOUSE_MAINTENANCE_ROUTE",
    "SBLR_LIFECYCLE_VERIFY_DATABASE",
    "SBLR_LIFECYCLE_REPAIR_DATABASE",
    "EngineVerifyLifecycle",
    "EngineRepairLifecycle",
)

FORBIDDEN_DONOR_SPECIFIC_SNIPPETS = {
    "postgresql": ("cluster.admin.run_maintenance",),
}

FORBIDDEN_LEGACY_DIAGNOSTICS = (
    "CASSANDRA.AUTHORITY.NODETOOL_DENIED",
    "CASSANDRA.AUTHORITY.REPAIR_DENIED",
    "DUCKDB.AUTHORITY.CHECKPOINT_DENIED",
    "CLICKHOUSE.AUTHORITY.SYSTEM_DENIED",
    "MYSQL.EMULATION.MAINTENANCE_ROUTE",
    "MARIADB.EMULATION.MAINTENANCE_ROUTE",
    "POSTGRESQL.EMULATION.MAINTENANCE_ROUTE",
    "SQLITE.EMULATION.MAINTENANCE_ROUTE",
    "CLICKHOUSE.EMULATION.MAINTENANCE_ROUTE",
)


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def read_csv(path: pathlib.Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        rows = list(reader)
    require(reader.fieldnames is not None, f"{path} has no header")
    missing = sorted(REQUIRED_COLUMNS - set(reader.fieldnames))
    require(not missing, f"{path} missing columns: {missing}")
    return rows


def current_donor_parser_dirs(repo_root: pathlib.Path) -> set[str]:
    root = repo_root / "project/src/parsers/donor"
    return {
        path.name
        for path in root.iterdir()
        if path.is_dir() and path.name != "common"
    }


def dialect_source_path(repo_root: pathlib.Path, donor_id: str) -> pathlib.Path:
    source_dir = repo_root / "project/src/parsers/donor" / donor_id
    matches = sorted(source_dir.glob("*_dialect.cpp"))
    require(matches, f"{donor_id} has no dialect source")
    require(len(matches) == 1, f"{donor_id} has ambiguous dialect sources: {matches}")
    return matches[0]


def validate_spec(repo_root: pathlib.Path) -> None:
    text = (repo_root / SPEC_REL).read_text(encoding="utf-8")
    normalized = " ".join(text.split())
    for phrase in REQUIRED_PHRASES:
        require(phrase in normalized, f"policy spec missing required phrase: {phrase}")


def validate_matrix(repo_root: pathlib.Path) -> dict[str, object]:
    rows = read_csv(repo_root / MATRIX_REL)
    parser_dirs = current_donor_parser_dirs(repo_root)
    by_donor: dict[str, dict[str, str]] = {}
    unsupported_rows: list[str] = []

    for row in rows:
        donor = row["donor_id"]
        require(donor not in by_donor, f"duplicate donor row: {donor}")
        by_donor[donor] = row
        for column in REQUIRED_COLUMNS:
            require(row[column].strip(), f"{donor} empty {column}")
        require(
            row["parser_package"] == f"project/src/parsers/donor/{donor}",
            f"{donor} parser package mismatch: {row['parser_package']}",
        )
        require(
            "SBsql-only" in row["sbsql_authority"],
            f"{donor} row must identify SBsql-only authority",
        )

        source_text = dialect_source_path(repo_root, donor).read_text(encoding="utf-8")
        has_declared_denial = row["denied_surfaces"] != "none_currently_declared"
        if has_declared_denial:
            expected_code = EXPECTED_UNSUPPORTED_DENIALS.get(donor)
            require(expected_code is not None, f"{donor} missing expected denial code")
            require(
                row["required_message_vector_code"] == expected_code,
                f"{donor} matrix code mismatch: {row['required_message_vector_code']}",
            )
            require(
                row["donor_parser_policy"] == "unsupported_denied",
                f"{donor} low-level utility policy must be unsupported_denied",
            )
            require(expected_code in source_text, f"{donor} source missing {expected_code}")
            require(
                "fail_closed" in row["parser_runtime_requirement"],
                f"{donor} runtime requirement must be fail closed",
            )
            unsupported_rows.append(donor)
        else:
            require(
                row["required_message_vector_code"] == "not_applicable",
                f"{donor} no-declared row must use not_applicable diagnostic code",
            )
            require(
                row["donor_parser_policy"] == "no_declared_low_level_utility_surface",
                f"{donor} no-declared row must use no_declared_low_level_utility_surface",
            )

    missing = sorted(parser_dirs - set(by_donor))
    extra = sorted(set(by_donor) - parser_dirs)
    require(not missing, f"matrix missing current donor parser rows: {missing}")
    require(not extra, f"matrix has rows for non-current donor parser dirs: {extra}")
    require(
        set(unsupported_rows) == set(EXPECTED_UNSUPPORTED_DENIALS),
        "unsupported-denied donor set mismatch: "
        f"matrix={sorted(unsupported_rows)} expected={sorted(EXPECTED_UNSUPPORTED_DENIALS)}",
    )
    return {
        "rows": len(rows),
        "donors": sorted(by_donor),
        "unsupported_denied_donors": sorted(unsupported_rows),
    }


def validate_source_policy(repo_root: pathlib.Path) -> dict[str, object]:
    donor_root = repo_root / "project/src/parsers/donor"
    source_text_by_path = {
        path: path.read_text(encoding="utf-8")
        for path in donor_root.glob("*/*_dialect.cpp")
    }
    combined = "\n".join(source_text_by_path.values())
    for snippet in FORBIDDEN_DONOR_SOURCE_SNIPPETS:
        require(snippet not in combined, f"donor source still contains forbidden route: {snippet}")
    for diagnostic in FORBIDDEN_LEGACY_DIAGNOSTICS:
        require(diagnostic not in combined, f"donor source still contains legacy diagnostic: {diagnostic}")
    for donor, snippets in FORBIDDEN_DONOR_SPECIFIC_SNIPPETS.items():
        source_text = dialect_source_path(repo_root, donor).read_text(encoding="utf-8")
        for snippet in snippets:
            require(snippet not in source_text, f"{donor} source still contains stale route: {snippet}")

    return {
        "source_files_scanned": len(source_text_by_path),
        "forbidden_route_snippets": list(FORBIDDEN_DONOR_SOURCE_SNIPPETS),
        "forbidden_donor_specific_snippets": FORBIDDEN_DONOR_SPECIFIC_SNIPPETS,
        "forbidden_legacy_diagnostics": list(FORBIDDEN_LEGACY_DIAGNOSTICS),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--evidence-file")
    args = parser.parse_args()

    repo_root = pathlib.Path(args.repo_root).resolve()
    validate_spec(repo_root)
    matrix_summary = validate_matrix(repo_root)
    source_summary = validate_source_policy(repo_root)
    summary = {
        "policy": "donor_low_level_utility_unsupported_denied",
        "matrix": matrix_summary,
        "source": source_summary,
    }
    if args.evidence_file:
        evidence_path = pathlib.Path(args.evidence_file)
        evidence_path.parent.mkdir(parents=True, exist_ok=True)
        evidence_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n",
                                 encoding="utf-8")
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
