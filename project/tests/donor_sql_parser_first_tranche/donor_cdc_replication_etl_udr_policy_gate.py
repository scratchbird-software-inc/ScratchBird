#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Gate for donor CDC, replication, and ETL parser-support UDR policy."""

from __future__ import annotations

import argparse
import csv
import json
import pathlib
import sys


SPEC_REL = (
    "project/tests/donor_sql_parser_first_tranche/"
    "CDC_REPLICATION_ETL_UDR_POLICY.md"
)
MATRIX_REL = (
    "project/tests/donor_sql_parser_first_tranche/"
    "cdc_replication_etl_udr_policy_matrix.csv"
)

REQUIRED_COLUMNS = {
    "donor_id",
    "display_name",
    "parser_package",
    "cdc_methods",
    "replication_methods",
    "etl_methods",
    "required_udr_diagnostics",
    "parser_boundary",
    "denied_or_separate_authority",
    "proof_tests",
    "notes",
}

REQUIRED_PHRASES = (
    "CDC",
    "replication",
    "ETL",
    "parser-support UDR",
    "donor parser must recognize",
    "MGA transaction and recovery authority remain engine-owned",
    "Server-local file access remains deny-by-default",
    "Cluster-specific commands are separate",
    "DONOR_CDC_REPLICATION_ETL_UDR_POLICY",
)

FORBIDDEN_DONOR_SOURCE_SNIPPETS = (
    "cluster.replication.consume_cluster_event",
    "sblr.replication.consumer",
    "cluster.metrics.emit_event",
    "sblr.cluster.report.v3:cluster.metrics.emit_event",
    "EngineStreamAppend",
    "EngineStreamRead",
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


def split_diagnostics(value: str) -> list[str]:
    if value == "not_applicable":
        return []
    return [part.strip() for part in value.split(";") if part.strip()]


def validate_matrix(repo_root: pathlib.Path) -> dict[str, object]:
    rows = read_csv(repo_root / MATRIX_REL)
    parser_dirs = current_donor_parser_dirs(repo_root)
    by_donor: dict[str, dict[str, str]] = {}
    udr_donors: list[str] = []
    diagnostics_by_donor: dict[str, list[str]] = {}

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
            "donor_cdc_replication_etl_udr_policy_gate" in row["proof_tests"],
            f"{donor} row must include this policy gate in proof_tests",
        )
        denied_lower = row["denied_or_separate_authority"].lower()
        require(
            any(token in denied_lower for token in ("denied", "separate", "cluster", "authority")),
            f"{donor} row must describe denied or separate authority",
        )

        diagnostics = split_diagnostics(row["required_udr_diagnostics"])
        diagnostics_by_donor[donor] = diagnostics
        source_text = dialect_source_path(repo_root, donor).read_text(encoding="utf-8")
        if diagnostics:
            udr_donors.append(donor)
            require(
                "donor UDR" in row["parser_boundary"],
                f"{donor} UDR row must name donor UDR parser boundary",
            )
            has_parser_support_udr_route = (
                "MappingDisposition::kParserSupportUdr" in source_text
                or "FirebirdMappingDisposition::kParserSupportUdr" in source_text
            )
            require(
                has_parser_support_udr_route,
                f"{donor} source has diagnostics but no parser-support UDR routes",
            )
            for diagnostic in diagnostics:
                require(
                    diagnostic in source_text,
                    f"{donor} source missing required UDR diagnostic {diagnostic}",
                )
        else:
            require(
                row["required_udr_diagnostics"] == "not_applicable",
                f"{donor} no-diagnostic row must use not_applicable",
            )

    missing = sorted(parser_dirs - set(by_donor))
    extra = sorted(set(by_donor) - parser_dirs)
    require(not missing, f"matrix missing current donor parser rows: {missing}")
    require(not extra, f"matrix has rows for non-current donor parser dirs: {extra}")
    require(udr_donors, "matrix declared no donor UDR CDC/replication/ETL rows")

    return {
        "rows": len(rows),
        "donors": sorted(by_donor),
        "udr_donors": sorted(udr_donors),
        "diagnostics_by_donor": {
            donor: diagnostics_by_donor[donor]
            for donor in sorted(diagnostics_by_donor)
        },
    }


def validate_source_policy(repo_root: pathlib.Path) -> dict[str, object]:
    donor_root = repo_root / "project/src/parsers/donor"
    source_text_by_path = {
        path: path.read_text(encoding="utf-8")
        for path in donor_root.glob("*/*_dialect.cpp")
    }
    combined = "\n".join(source_text_by_path.values())
    for snippet in FORBIDDEN_DONOR_SOURCE_SNIPPETS:
        require(
            snippet not in combined,
            f"donor source still contains forbidden direct CDC/replication/ETL route: {snippet}",
        )

    return {
        "source_files_scanned": len(source_text_by_path),
        "forbidden_direct_route_snippets": list(FORBIDDEN_DONOR_SOURCE_SNIPPETS),
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
        "policy": "donor_cdc_replication_etl_udr",
        "matrix": matrix_summary,
        "source": source_summary,
    }
    if args.evidence_file:
        evidence_path = pathlib.Path(args.evidence_file)
        evidence_path.parent.mkdir(parents=True, exist_ok=True)
        evidence_path.write_text(
            json.dumps(summary, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
