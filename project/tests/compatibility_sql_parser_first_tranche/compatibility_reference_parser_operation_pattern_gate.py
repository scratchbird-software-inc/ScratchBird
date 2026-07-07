#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Reference parser operation-pattern authority gate.

Every table-driven compatibility parser route must either produce an SBLR/UUID
operation for engine admission or an exact fail-closed diagnostic. Firebird uses
a custom lifecycle mapping table, so it is checked for equivalent custom mapping
metadata instead of the shared OperationPattern table.
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


TABLE_DRIVEN_LANES = (
    "apache_ignite",
    "cassandra",
    "clickhouse",
    "cockroachdb",
    "dolt",
    "duckdb",
    "foundationdb",
    "immudb",
    "influxdb",
    "mariadb",
    "milvus",
    "mongodb",
    "mysql",
    "neo4j",
    "opensearch",
    "opensearch_sql_ppl",
    "postgresql",
    "redis",
    "sqlite",
    "tidb",
    "tikv",
    "vitess",
    "xtdb",
    "yugabytedb",
)

ROUTED_DISPOSITIONS = {
    "AdmittedSblr",
    "ScratchBirdLifecycleApi",
    "ParserSupportUdr",
    "CatalogProjection",
}

FAIL_CLOSED_DISPOSITIONS = {
    "PolicyRefusal",
    "SecurityRefusal",
    "UnsupportedRefusal",
}

PATTERN_RE = re.compile(
    r'\{"(?P<match>[^"]*)",\s*PatternMatch::k(?P<match_kind>\w+),\s*'
    r'"(?P<statement_family>[^"]*)",\s*"(?P<operation_family>[^"]*)",\s*'
    r'MappingDisposition::k(?P<disposition>\w+),\s*"(?P<mapping_key>[^"]*)",\s*'
    r'"(?P<sblr_operation>[^"]*)",\s*"(?P<engine_api_function>[^"]*)",\s*'
    r'"(?P<diagnostic_code>[^"]*)",\s*"(?P<diagnostic_message>[^"]*)",\s*'
    r'(?P<requires_security_context>true|false),\s*'
    r'(?P<requires_transaction_context>true|false)\}',
    re.MULTILINE | re.DOTALL,
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def parse_patterns(source: Path) -> list[dict[str, str]]:
    text = source.read_text(encoding="utf-8")
    rows = [match.groupdict() for match in PATTERN_RE.finditer(text)]
    require(rows, f"{source}: no OperationPattern rows parsed")
    return rows


def verify_table_lane(repo_root: Path, lane: str) -> dict[str, object]:
    source = repo_root / f"project/src/parsers/compatibility/{lane}/{lane}_dialect.cpp"
    rows = parse_patterns(source)
    routed = 0
    fail_closed = 0
    parser_support = 0
    catalog_projection = 0
    seen_operation_families: set[str] = set()

    for row in rows:
        disposition = row["disposition"]
        operation_family = row["operation_family"]
        statement_family = row["statement_family"]
        match_text = row["match"]
        require(match_text, f"{lane}: empty match text in {row}")
        require(statement_family, f"{lane}: empty statement family in {row}")
        require(operation_family.startswith(f"{lane}."),
                f"{lane}: operation family must be lane-scoped: {operation_family}")
        require(row["mapping_key"], f"{lane}: empty mapping key in {row}")
        require(disposition in ROUTED_DISPOSITIONS | FAIL_CLOSED_DISPOSITIONS,
                f"{lane}: unknown disposition {disposition}")
        seen_operation_families.add(operation_family)

        if disposition in ROUTED_DISPOSITIONS:
            routed += 1
            require(row["sblr_operation"].startswith("SBLR_") or
                    row["sblr_operation"].startswith("sblr.") or
                    "sblr." in row["sblr_operation"],
                    f"{lane}: routed pattern missing SBLR operation in {operation_family}")
            require(row["engine_api_function"],
                    f"{lane}: routed pattern missing engine/parser API function in {operation_family}")
            if disposition == "ParserSupportUdr":
                parser_support += 1
            if disposition == "CatalogProjection":
                catalog_projection += 1
        else:
            fail_closed += 1
            require(row["diagnostic_code"].startswith(f"{lane.upper()}.") or
                    row["diagnostic_code"].startswith(lane.replace("_", "").upper()) or
                    row["diagnostic_code"].startswith("OPENSEARCH") or
                    row["diagnostic_code"].startswith("MONGO") or
                    row["diagnostic_code"].startswith("FDB"),
                    f"{lane}: fail-closed diagnostic is not lane scoped in {operation_family}")
            require(row["diagnostic_message"],
                    f"{lane}: fail-closed pattern missing diagnostic message in {operation_family}")

    require(routed > 0, f"{lane}: no routed patterns")
    require(parser_support > 0 or catalog_projection > 0 or fail_closed > 0,
            f"{lane}: no parser-support/catalog/refusal policy surfaces")
    return {
        "lane": lane,
        "pattern_count": len(rows),
        "unique_operation_families": len(seen_operation_families),
        "routed_count": routed,
        "parser_support_count": parser_support,
        "catalog_projection_count": catalog_projection,
        "fail_closed_count": fail_closed,
    }


def verify_firebird(repo_root: Path) -> dict[str, object]:
    source = repo_root / "project/src/parsers/compatibility/firebird/firebird_dialect.cpp"
    text = source.read_text(encoding="utf-8")
    mapping_rows = len(re.findall(r'FirebirdLifecycleMappingDescriptor', text))
    for fragment in (
        "SBLR_LIFECYCLE_CREATE_DATABASE",
        "sblr.compatibility.firebird.logical_stream.v1",
        "FIREBIRD.LOGICAL_STREAM.GBAK_REMOTE_STREAM",
        "FIREBIRD.AUTHORITY.UNSUPPORTED_DENIED",
        "parser_transaction_finality_authority",
        "reference_engine_sql_executed",
    ):
        require(fragment in text, f"firebird: missing custom mapping fragment {fragment}")
    require(mapping_rows >= 2, "firebird: lifecycle mapping descriptor table missing")
    return {
        "lane": "firebird",
        "parser_kind": "custom_firebird_mapping",
        "mapping_descriptor_mentions": mapping_rows,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--evidence-file", required=True, type=Path)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    lanes = [verify_firebird(repo_root)]
    lanes.extend(verify_table_lane(repo_root, lane) for lane in TABLE_DRIVEN_LANES)

    evidence = {
        "gate": "compatibility_reference_parser_operation_pattern_gate",
        "status": "passed",
        "lane_count": len(lanes),
        "engine_authority_policy": "engine_sblr_mga_only",
        "file_presence_is_completion": False,
        "lanes": lanes,
    }
    args.evidence_file.parent.mkdir(parents=True, exist_ok=True)
    args.evidence_file.write_text(
        json.dumps(evidence, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(f"compatibility_reference_parser_operation_pattern_gate=passed lanes={len(lanes)}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"compatibility_reference_parser_operation_pattern_gate: {exc}")
        raise SystemExit(1)
