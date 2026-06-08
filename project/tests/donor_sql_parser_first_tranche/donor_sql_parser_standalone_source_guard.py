#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Guard donor parser modules against cross-dialect implementation drift."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


DIALECTS = (
    "firebird",
    "mysql",
    "postgresql",
    "sqlite",
    "mariadb",
    "duckdb",
    "clickhouse",
    "tidb",
    "vitess",
    "cockroachdb",
    "yugabytedb",
    "cassandra",
    "mongodb",
    "redis",
    "opensearch_sql_ppl",
    "opensearch",
    "neo4j",
    "influxdb",
    "milvus",
    "dolt",
    "apache_ignite",
    "tikv",
    "foundationdb",
    "immudb",
    "xtdb",
)
SOURCE_SUFFIXES = (".cpp", ".hpp")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def allowed_hit(dialect: str, forbidden: str, line: str) -> bool:
    if dialect == "mariadb" and forbidden == "mysql":
        return ("MYSQL." in line or "MYSQL_" in line or "mysql_schema" in line or
                ("MYSQL" in line and "catalog_overlays" in line))
    if dialect == "opensearch_sql_ppl" and forbidden == "opensearch":
        return "OpenSearch SQL/PPL" in line or "opensearch_sql_ppl" in line
    return False


def forbidden_tokens_for(root_kind: str, dialect: str) -> list[str]:
    forbidden = [name for name in DIALECTS if name != dialect]
    if root_kind == "parser":
        forbidden.append("sbsql")
    return forbidden


def source_roots(repo_root: Path, dialect: str) -> list[tuple[str, Path]]:
    parser_root = repo_root / "project/src/parsers/donor" / dialect
    udr_root = repo_root / "project/src/udr" / f"sbu_{dialect}_parser_support"
    require(parser_root.is_dir(), f"missing parser module: {parser_root}")
    require(udr_root.is_dir(), f"missing parser-support UDR module: {udr_root}")
    return [("parser", parser_root), ("udr", udr_root)]


def scan_source_root(repo_root: Path, dialect: str, root_kind: str,
                     root_path: Path) -> list[dict[str, object]]:
    forbidden = forbidden_tokens_for(root_kind, dialect)
    pattern = re.compile(r"\b(" + "|".join(re.escape(name) for name in forbidden) + r")\b",
                         re.IGNORECASE)
    violations: list[dict[str, object]] = []
    for path in sorted(root_path.rglob("*")):
        if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
            continue
        text = path.read_text(encoding="utf-8")
        for line_no, line in enumerate(text.splitlines(), start=1):
            for match in pattern.finditer(line):
                hit = match.group(1).lower()
                if allowed_hit(dialect, hit, line):
                    continue
                violations.append({
                    "dialect": dialect,
                    "source_root": root_kind,
                    "path": path.relative_to(repo_root).as_posix(),
                    "line": line_no,
                    "forbidden_donor_token": hit,
                })
    return violations


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--evidence-file", required=True, type=Path)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    all_violations: list[dict[str, object]] = []
    for dialect in DIALECTS:
        for root_kind, root_path in source_roots(repo_root, dialect):
            all_violations.extend(scan_source_root(repo_root, dialect, root_kind, root_path))
    if all_violations:
        raise AssertionError(json.dumps(all_violations, indent=2, sort_keys=True))

    evidence = {
        "gate": "donor_sql_parser_standalone_source_guard",
        "status": "passed",
        "dialects": list(DIALECTS),
        "source_roots_checked": ["project/src/parsers/donor/<dialect>",
                                 "project/src/udr/sbu_<dialect>_parser_support"],
        "parser_modules_are_standalone": True,
        "parser_support_udr_modules_are_standalone": True,
        "cross_donor_detection_or_dispatch_allowed": False,
        "sbsql_surface_allowed_in_donor_parser": False,
        "sbsql_package_metadata_allowed_in_parser_support_udr": True,
    }
    args.evidence_file.parent.mkdir(parents=True, exist_ok=True)
    args.evidence_file.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n",
                                  encoding="utf-8")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"donor_sql_parser_standalone_source_guard: {exc}", file=sys.stderr)
        raise SystemExit(1)
