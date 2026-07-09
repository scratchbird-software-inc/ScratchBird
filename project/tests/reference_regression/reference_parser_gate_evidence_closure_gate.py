#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate reference parser implementation-start handoff evidence.

This gate is intentionally public-repo local. It proves that each reference
parser lane has source-independent manifests, original regression-suite
inventory, native-tool replay requirements, and ScratchBird authority
boundaries before implementation starts. It does not mark a parser complete.
Completion still requires actual replay through the original test tools and
reference regression suites.
"""

from __future__ import annotations

import argparse
import csv
import pathlib
import sys


REFERENCE_ROOT = pathlib.Path("project/tests/reference_regression")
EXPECTED_REFERENCE_ROOTS = (
    ("apache_ignite", "apache_ignite"),
    ("cassandra", "cassandra"),
    ("clickhouse", "clickhouse"),
    ("cockroachdb", "cockroachdb"),
    ("dolt", "dolt"),
    ("duckdb", "duckdb"),
    ("firebird", "firebird"),
    ("foundationdb", "foundationdb"),
    ("immudb", "immudb"),
    ("influxdb", "influxdb"),
    ("mariadb", "mariadb"),
    ("milvus", "milvus"),
    ("mongodb", "mongodb"),
    ("mysql", "mysql"),
    ("neo4j", "neo4j"),
    ("opensearch_rest", "opensearch/rest_dsl"),
    ("opensearch_sql_ppl", "opensearch_sql_ppl"),
    ("postgresql", "postgresql"),
    ("redis", "redis"),
    ("sqlite", "sqlite"),
    ("tidb", "tidb"),
    ("tikv", "tikv"),
    ("vitess", "vitess"),
    ("xtdb", "xtdb"),
    ("yugabytedb", "yugabytedb"),
)
EXPECTED_TOP_LEVEL_REFERENCE_ROOTS = (
    "apache_ignite",
    "cassandra",
    "clickhouse",
    "cockroachdb",
    "dolt",
    "duckdb",
    "firebird",
    "foundationdb",
    "immudb",
    "influxdb",
    "mariadb",
    "milvus",
    "mongodb",
    "mysql",
    "neo4j",
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

EVIDENCE_FILES = {
    "reference_regression_inventory": "upstream_manifest.csv",
    "native_tool_replay": "native_tool_harness/native_tool_harness_manifest.csv",
    "security_policy": "security_operations/security_policy_manifest.csv",
    "catalog_policy": "catalog_policy/catalog_policy_manifest.csv",
    "migration_policy": "operations_migration/migration_policy_manifest.csv",
    "performance_baseline": "performance/performance_baseline_manifest.csv",
    "wire_transcripts": "wire_transcripts/wire_transcript_manifest.csv",
    "resource_limits": "resource_limits/resource_limit_manifest.csv",
    "management_package_abi": "management_package_abi/management_package_abi_manifest.csv",
    "release_evidence": "release_evidence/release_evidence_manifest.csv",
    "version_compatibility": "compatibility/version_compatibility_manifest.csv",
    "compatibility_variance": "compatibility_variance/compatibility_variance_manifest.csv",
    "cross_dialect": "cross_dialect/cross_dialect_manifest.csv",
    "enterprise_completion": "enterprise_completion/enterprise_completion_manifest.csv",
    "fixtures": "fixtures/fixture_manifest.csv",
    "goldens": "goldens/golden_manifest.csv",
    "policy_overlay": "policy/policy_overlay_manifest.csv",
}

GENERATED_OR_START_READY_TOKENS = (
    "manifest_generated",
    "locator_corrected",
    "accepted_for_implementation_start",
    "go_for_parser_implementation_start",
)


def read_csv(path: pathlib.Path) -> list[dict[str, str]]:
    if not path.is_file():
        raise AssertionError(f"missing required CSV: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None:
            raise AssertionError(f"{path}: missing CSV header")
        rows: list[dict[str, str]] = []
        for row in reader:
            row.pop(None, None)
            rows.append(dict(row))
    if not rows:
        raise AssertionError(f"{path}: no rows")
    return rows


def status_is_start_ready(status: str) -> bool:
    if status in {
        "completed",
        "passed",
        "passing",
        "implemented_in_full",
        "no_exclusions_accepted",
        "enterprise_blocked_pending_dpde_closure",
    }:
        return True
    if "_passed" in status or status.endswith("_closed"):
        return True
    return any(token in status for token in GENERATED_OR_START_READY_TOKENS)


def validate_reference(repo: pathlib.Path, reference_id: str, reference_root: str) -> None:
    root = repo / REFERENCE_ROOT / reference_root
    if not root.is_dir():
        raise AssertionError(f"{reference_id}: missing reference regression root")

    for family, rel in EVIDENCE_FILES.items():
        rows = read_csv(root / rel)
        statuses = {row.get("status", "") for row in rows}
        if not any(status_is_start_ready(status) for status in statuses):
            raise AssertionError(f"{reference_id}:{family}: no start-ready status in {sorted(statuses)}")

    upstream = read_csv(root / "upstream_manifest.csv")
    missing_sources = [
        row.get("suite_id", row.get("reference_id", reference_id))
        for row in upstream
        if row.get("source_exists") != "yes"
    ]
    if missing_sources:
        raise AssertionError(f"{reference_id}: missing source inventory rows: {', '.join(missing_sources)}")

    exclusions = read_csv(root / "exclusion_register.csv")
    silent_exclusions = [
        row.get("exclusion_id", reference_id)
        for row in exclusions
        if "mandatory_reason_code" not in row.get("reason_code", "")
        and not status_is_start_ready(row.get("status", ""))
    ]
    if silent_exclusions:
        raise AssertionError(f"{reference_id}: unclassified exclusions: {', '.join(silent_exclusions)}")

    native = read_csv(root / "native_tool_harness/native_tool_harness_manifest.csv")
    for row in native:
        locator = row.get("tool_locator", "")
        if row.get("tool_exists") == "yes":
            if not locator.startswith((REFERENCE_ROOT / reference_root / "native_tool_harness/tools").as_posix()):
                raise AssertionError(f"{reference_id}: native tool locator is outside the harness: {locator}")
            # Native reference-tool binaries are external local replay fixtures.
            # The public repo tracks the locator and endpoint contract, while
            # acquisition/replay gates skip until those payloads are installed.
        else:
            if locator not in {"no_tool_recorded", "no_compiled_tool_packaged"}:
                raise AssertionError(f"{reference_id}: missing native tool has unclear locator: {locator}")
        for column in ("required_endpoint_env", "required_output", "parser_authority_rule"):
            if not row.get(column):
                raise AssertionError(f"{reference_id}: native tool row missing {column}")


def validate(repo: pathlib.Path) -> None:
    if (repo / "docs/execution-plans").exists():
        raise AssertionError("public repo must not contain docs/execution-plans workplan material")

    present = {
        path.name
        for path in (repo / REFERENCE_ROOT).iterdir()
        if path.is_dir() and path.name not in {"__pycache__", "fixtures", "reference_catalog_seeds", "reference_release_acquisition", "scratchbird", "opensearch"}
    }
    expected = set(EXPECTED_TOP_LEVEL_REFERENCE_ROOTS)
    missing = sorted(expected - present)
    extra = sorted(present - expected)
    if missing:
        raise AssertionError(f"missing reference parser test roots: {', '.join(missing)}")
    if extra:
        raise AssertionError(f"unexpected reference parser test roots: {', '.join(extra)}")

    for reference_id, reference_root in EXPECTED_REFERENCE_ROOTS:
        validate_reference(repo, reference_id, reference_root)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=pathlib.Path)
    args = parser.parse_args(argv)
    validate(args.repo_root.resolve())
    print(f"reference_parser_implementation_start_handoff=pass references={len(EXPECTED_REFERENCE_ROOTS)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
