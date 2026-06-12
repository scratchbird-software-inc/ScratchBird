#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Gate for compatibility logical remote-stream backup/restore policy coverage."""

from __future__ import annotations

import argparse
import csv
import json
import pathlib
import sys


SPEC_REL = (
    "project/tests/compatibility_sql_parser_first_tranche/"
    "LOGICAL_STREAM_BACKUP_RESTORE_POLICY.md"
)
MATRIX_REL = (
    "project/tests/compatibility_sql_parser_first_tranche/"
    "logical_stream_backup_restore_policy_matrix.csv"
)
OPTIONAL_PRIVATE_SPEC_REL = (
    "public_contract_snapshot"
    "appendix-compatibility-logical-remote-stream-backup-restore-policy.md"
)
OPTIONAL_PRIVATE_MATRIX_REL = (
    "public_contract_snapshot"
    "compatibility-logical-remote-stream-backup-restore-policy-matrix.csv"
)

REQUIRED_COLUMNS = {
    "compatibility_id",
    "display_name",
    "parser_package",
    "full_logical_restore_stream",
    "partial_logical_restore_stream",
    "full_logical_backup_stream",
    "partial_logical_backup_stream",
    "allowed_inbound_streams",
    "allowed_outbound_streams",
    "denied_inbound",
    "denied_outbound",
    "sblr_requirement",
    "parser_role",
    "engine_authority",
    "default_server_file_policy",
    "physical_backup_policy",
    "connected_database_scope",
    "notes",
}
POLICY_VALUES = {"allowed", "conditional", "denied"}
REQUIRED_PHRASES = (
    "remote client stream",
    "logical",
    "single connected emulated compatibility database",
    "server-local file",
    "physical page-copy backup",
    "nbackup",
    "gbak",
    "required_logical_stream_backup_restore_surface",
)
RELATIONAL_CLIENT_STREAM_COMPATIBILITYS = {
    "mysql",
    "mariadb",
    "postgresql",
    "tidb",
    "vitess",
    "yugabytedb",
}
COPY_LOGICAL_STREAM_SOURCE_REQUIREMENTS = {
    "postgresql": {
        "path": "project/src/parsers/compatibility/postgresql/postgresql_dialect.cpp",
        "from_stdin": "postgresql.logical_stream.copy_from_stdin",
        "to_stdout": "postgresql.logical_stream.copy_to_stdout",
        "sblr": "SBLR_COMPATIBILITY_POSTGRESQL_COPY_ROUTE",
    },
    "cockroachdb": {
        "path": "project/src/parsers/compatibility/cockroachdb/cockroachdb_dialect.cpp",
        "from_stdin": "cockroachdb.logical_stream.copy_from_stdin",
        "to_stdout": "cockroachdb.logical_stream.copy_to_stdout",
        "sblr": "SBLR_COMPATIBILITY_COCKROACHDB_COPY_ROUTE",
    },
    "yugabytedb": {
        "path": "project/src/parsers/compatibility/yugabytedb/yugabytedb_dialect.cpp",
        "from_stdin": "yugabytedb.logical_stream.copy_from_stdin",
        "to_stdout": "yugabytedb.logical_stream.copy_to_stdout",
        "sblr": "SBLR_COMPATIBILITY_YUGABYTEDB_COPY_ROUTE",
    },
}


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


def current_compatibility_parser_dirs(repo_root: pathlib.Path) -> set[str]:
    root = repo_root / "project/src/parsers/compatibility"
    return {
        path.name
        for path in root.iterdir()
        if path.is_dir() and path.name != "common"
    }


def validate_spec_text(repo_root: pathlib.Path) -> None:
    spec = (repo_root / SPEC_REL).read_text(encoding="utf-8")
    lower = " ".join(spec.lower().split())
    for phrase in REQUIRED_PHRASES:
        require(phrase.lower() in lower, f"spec missing required phrase: {phrase}")

    private_spec = repo_root / OPTIONAL_PRIVATE_SPEC_REL
    if private_spec.exists():
        private_lower = " ".join(
            private_spec.read_text(encoding="utf-8").lower().split()
        )
        for phrase in REQUIRED_PHRASES:
            require(
                phrase.lower() in private_lower,
                f"private spec missing required phrase: {phrase}",
            )


def validate_matrix(repo_root: pathlib.Path) -> dict[str, object]:
    rows = read_csv(repo_root / MATRIX_REL)
    private_matrix = repo_root / OPTIONAL_PRIVATE_MATRIX_REL
    if private_matrix.exists():
        private_rows = read_csv(private_matrix)
        require(
            sorted(row["compatibility_id"] for row in private_rows)
            == sorted(row["compatibility_id"] for row in rows),
            "private matrix compatibility set differs from tracked test matrix",
        )
    parser_dirs = current_compatibility_parser_dirs(repo_root)
    by_compatibility: dict[str, dict[str, str]] = {}
    for row in rows:
        compatibility = row["compatibility_id"]
        require(compatibility not in by_compatibility, f"duplicate compatibility row: {compatibility}")
        by_compatibility[compatibility] = row
        for column in REQUIRED_COLUMNS:
            require(row[column].strip(), f"{compatibility} empty {column}")
        for column in (
            "full_logical_restore_stream",
            "partial_logical_restore_stream",
            "full_logical_backup_stream",
            "partial_logical_backup_stream",
        ):
            require(
                row[column] in POLICY_VALUES,
                f"{compatibility} invalid policy value {column}={row[column]}",
            )
        require(
            row["parser_package"] == f"project/src/parsers/compatibility/{compatibility}",
            f"{compatibility} parser package mismatch: {row['parser_package']}",
        )
        require(
            row["sblr_requirement"] == "required_logical_stream_backup_restore_surface",
            f"{compatibility} missing required logical-stream SBLR requirement",
        )
        require(
            row["default_server_file_policy"] == "deny_by_default",
            f"{compatibility} must deny server-local files by default",
        )
        require(
            row["physical_backup_policy"].startswith("deny_"),
            f"{compatibility} must deny physical/page-copy backup policy",
        )
        require(
            row["connected_database_scope"] == "single_connected_legacy_database_only",
            f"{compatibility} must be scoped to one connected legacy database",
        )
        denied = f"{row['denied_inbound']} {row['denied_outbound']}".lower()
        require(
            any(token in denied for token in ("server-local", "snapshot", "physical", "page-copy", "backup")),
            f"{compatibility} denial text must cover server-local or physical authority",
        )
        authority = row["engine_authority"].lower()
        require("mga" in authority, f"{compatibility} engine authority must mention MGA")

    missing = sorted(parser_dirs - set(by_compatibility))
    extra = sorted(set(by_compatibility) - parser_dirs)
    require(not missing, f"matrix missing current compatibility parser rows: {missing}")
    require(not extra, f"matrix has rows for non-current compatibility parser dirs: {extra}")

    firebird = by_compatibility["firebird"]
    firebird_text = " ".join(firebird.values()).lower()
    require("gbak" in firebird_text, "firebird row must explicitly allow gbak stream")
    require("nbackup" in firebird_text, "firebird row must explicitly deny nbackup")

    for compatibility in RELATIONAL_CLIENT_STREAM_COMPATIBILITYS:
        text = " ".join(by_compatibility[compatibility].values()).lower()
        require(
            any(token in text for token in ("stdin", "stdout", "local client", "client stream", "copy")),
            f"{compatibility} row must distinguish client logical stream",
        )
        require(
            "server-local" in text,
            f"{compatibility} row must deny server-local file access",
        )

    return {
        "rows": len(rows),
        "compatibilitys": sorted(by_compatibility),
        "allowed_or_conditional_full_restore": sum(
            1 for row in rows
            if row["full_logical_restore_stream"] in {"allowed", "conditional"}
        ),
        "allowed_or_conditional_full_backup": sum(
            1 for row in rows
            if row["full_logical_backup_stream"] in {"allowed", "conditional"}
        ),
        "server_file_policy": "deny_by_default",
        "physical_backup_policy": "deny_for_all_rows",
    }


def validate_copy_logical_stream_source(repo_root: pathlib.Path) -> dict[str, object]:
    verified: list[str] = []
    for compatibility, requirement in COPY_LOGICAL_STREAM_SOURCE_REQUIREMENTS.items():
        text = (repo_root / requirement["path"]).read_text(
            encoding="utf-8", errors="ignore"
        )
        for token in (
            "COPY|| TO STDOUT",
            "COPY|| FROM STDIN",
            "COPY|| TO '",
            "COPY|| FROM '",
            "COPY|| PROGRAM ",
            "PatternMatch::kPrefixAndContains",
            requirement["from_stdin"],
            requirement["to_stdout"],
            requirement["sblr"],
        ):
            require(token in text, f"{compatibility} COPY logical-stream source missing {token}")
        verified.append(compatibility)
    return {
        "copy_logical_stream_source_verified": verified,
        "copy_logical_stream_source_verified_count": len(verified),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--evidence-file")
    args = parser.parse_args()

    repo_root = pathlib.Path(args.repo_root).resolve()
    validate_spec_text(repo_root)
    summary = validate_matrix(repo_root)
    source_summary = validate_copy_logical_stream_source(repo_root)

    evidence = {
        "gate": "compatibility_logical_stream_backup_restore_policy_gate",
        "status": "passed",
        "spec": SPEC_REL,
        "matrix": MATRIX_REL,
        **summary,
        **source_summary,
    }
    if args.evidence_file:
        path = pathlib.Path(args.evidence_file)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n",
                        encoding="utf-8")
    print("compatibility_logical_stream_backup_restore_policy_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
