#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Full-lane command-vector proof for reference parser declarations.

This gate executes every table-driven OperationPattern row through the built
parser binaries. It proves the public parser declaration is not only present in
source, but produces the expected SBLR/UUID envelope or exact fail-closed
diagnostic at the product parser boundary.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import re
import subprocess
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

FIREBIRD_VECTORS = (
    {
        "sql": "CREATE DATABASE 'scratchbird_ref.fdb'",
        "operation_family": "firebird.emulated.database_lifecycle",
        "mapping_key": "firebird.lifecycle.create_database",
        "mapping_disposition": "scratchbird_lifecycle_api",
        "scratchbird_lifecycle_api": True,
    },
    {
        "sql": "DROP DATABASE",
        "operation_family": "firebird.emulated.database_lifecycle",
        "mapping_key": "firebird.lifecycle.drop_database",
        "mapping_disposition": "scratchbird_lifecycle_api",
        "scratchbird_lifecycle_api": True,
    },
    {
        "sql": "CONNECT 'scratchbird_ref.fdb'",
        "operation_family": "firebird.isql.connect",
        "mapping_key": "firebird.lifecycle.attach_database",
        "mapping_disposition": "scratchbird_lifecycle_api",
        "scratchbird_lifecycle_api": True,
    },
    {
        "sql": "DISCONNECT",
        "operation_family": "firebird.isql.disconnect",
        "mapping_key": "firebird.lifecycle.detach_database",
        "mapping_disposition": "scratchbird_lifecycle_api",
        "scratchbird_lifecycle_api": True,
    },
    {
        "sql": "VALIDATE DATABASE",
        "expect_exit": 1,
        "diagnostic_code": "FIREBIRD.AUTHORITY.UNSUPPORTED_DENIED",
        "operation_family": "firebird.emulated.validation_repair_sweep",
        "mapping_key": "firebird.lifecycle.verify_database",
    },
    {
        "sql": "REPAIR DATABASE",
        "expect_exit": 1,
        "diagnostic_code": "FIREBIRD.AUTHORITY.UNSUPPORTED_DENIED",
        "operation_family": "firebird.emulated.validation_repair_sweep",
        "mapping_key": "firebird.lifecycle.repair_database",
    },
    {
        "sql": "SWEEP DATABASE",
        "expect_exit": 1,
        "diagnostic_code": "FIREBIRD.AUTHORITY.UNSUPPORTED_DENIED",
        "operation_family": "firebird.emulated.validation_repair_sweep",
        "mapping_key": "firebird.unsupported.low_level_utility",
    },
    {
        "sql": "ALTER DATABASE ADD FILE 'scratchbird_ref_2.fdb'",
        "operation_family": "firebird.emulated.database_lifecycle",
        "mapping_key": "firebird.emulated.database_file_management",
        "mapping_disposition": "emulated_non_file_diagnostic",
        "exact_emulated_diagnostic": True,
    },
    {
        "sql": "CREATE SHADOW 1 'scratchbird_ref.shadow'",
        "operation_family": "firebird.emulated.shadow_storage",
        "mapping_key": "firebird.emulated.shadow_storage",
        "mapping_disposition": "emulated_non_file_diagnostic",
        "exact_emulated_diagnostic": True,
    },
    {
        "sql": "BACKUP DATABASE 'scratchbird_ref.fdb' TO 'scratchbird_ref.fbk'",
        "operation_family": "firebird.emulated.backup_restore",
        "mapping_key": "firebird.emulated.backup_restore",
        "mapping_disposition": "emulated_non_file_diagnostic",
        "exact_emulated_diagnostic": True,
    },
    {
        "sql": "NBACKUP -B scratchbird_ref.fdb scratchbird_ref.nbk",
        "expect_exit": 1,
        "diagnostic_code": "FIREBIRD.AUTHORITY.UNSUPPORTED_DENIED",
        "operation_family": "firebird.emulated.incremental_backup",
        "mapping_key": "firebird.emulated.backup_restore",
    },
    {
        "sql": "CREATE EXTERNAL TABLE ext EXTERNAL FILE 'scratchbird_ref.dat' (id integer)",
        "operation_family": "firebird.emulated.external_table_authority",
        "mapping_key": "firebird.emulated.external_plugin",
        "mapping_disposition": "emulated_non_file_diagnostic",
        "exact_emulated_diagnostic": True,
    },
    {
        "sql": "SERVICE MANAGER",
        "operation_family": "firebird.emulated.service_api",
        "mapping_key": "firebird.emulated.service_api",
        "mapping_disposition": "emulated_non_file_diagnostic",
        "exact_emulated_diagnostic": True,
    },
    {
        "sql": "CREATE JOURNAL scratchbird_ref",
        "operation_family": "firebird.emulated.replication_journal",
        "mapping_key": "firebird.emulated.replication_journal",
        "mapping_disposition": "parser_support_udr",
        "parser_support_udr_route": True,
    },
    {
        "sql": "GBAK -B stdin stdout",
        "operation_family": "firebird.logical_stream.gbak_backup",
        "mapping_key": "",
        "mapping_disposition": "",
    },
)

DISPOSITION_TO_ENVELOPE = {
    "AdmittedSblr": "admitted_sblr",
    "ScratchBirdLifecycleApi": "scratchbird_lifecycle_api",
    "ParserSupportUdr": "parser_support_udr",
    "CatalogProjection": "catalog_projection",
    "PolicyRefusal": "policy_refusal_fail_closed",
    "SecurityRefusal": "security_refusal_fail_closed",
    "UnsupportedRefusal": "unsupported_refusal_fail_closed",
}

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


def parser_binary(parser_bin_root: Path, lane: str) -> Path:
    binary = parser_bin_root / f"sbp_{lane}"
    candidates = [binary]
    if os.name == "nt" and binary.suffix.lower() != ".exe":
        candidates.append(binary.with_name(binary.name + ".exe"))
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return candidates[-1]


def read_manifest(path: Path) -> dict[str, dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        require(reader.fieldnames is not None, f"{path}: missing CSV header")
        return {row["family_id"]: dict(row) for row in reader}


def parse_patterns(source: Path) -> list[dict[str, str]]:
    text = source.read_text(encoding="utf-8")
    rows = [match.groupdict() for match in PATTERN_RE.finditer(text)]
    require(rows, f"{source}: no OperationPattern rows parsed")
    return rows


def first_alternative(value: str) -> str:
    return value.split("||", 1)[0].strip()


def balanced_single_quotes(value: str) -> str:
    if value.count("'") % 2 == 1:
        return value + "scratchbird_vector'"
    return value


def sql_for_pattern(row: dict[str, str]) -> str:
    match = row["match"]
    kind = row["match_kind"]
    if kind == "Prefix":
        return balanced_single_quotes(f"{match} sb_object")
    if kind == "Contains":
        return balanced_single_quotes(f"SELECT 1 {match} sb_object")
    if kind == "PrefixAndContains":
        prefix, fragment = match.split("||", 1)
        return balanced_single_quotes(f"{prefix} sb_object {fragment} sb_object")
    if kind == "ContainsFunctionCall":
        return f"SELECT {match}(1)"
    if kind == "LoadDataLocalInfile":
        return "LOAD DATA LOCAL INFILE 'scratchbird.csv' INTO TABLE sb_object"
    if kind == "LoadDataServerInfile":
        return "LOAD DATA INFILE 'scratchbird.csv' INTO TABLE sb_object"
    if kind == "CreateTableEngineClause":
        return f"CREATE TABLE sb_object (id integer) ENGINE={match}"
    if kind == "FromStringLiteralUriScheme":
        scheme = first_alternative(match).lower()
        return f"SELECT * FROM '{scheme}://scratchbird.example/vector'"
    if kind == "RestPathSegment":
        path = "/" + match.strip("/")
        return f"GET {path}/scratchbird_vector"
    if kind == "RestMethodRoute":
        route = first_alternative(match)
        if " " not in route:
            route = f"{route} /scratchbird_vector"
        return route
    if kind == "PplPipelineStage":
        return f"source=scratchbird | {match} field"
    if kind == "Word":
        return f"SELECT {match} FROM sb_object"
    if kind == "RelationReference":
        return f"SELECT * FROM {match}"
    raise AssertionError(f"unsupported PatternMatch kind: {kind}")


def parse_json_stdout(stdout: str, lane: str, sql: str) -> dict[str, object]:
    for line in stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            payload = json.loads(line)
        except json.JSONDecodeError as exc:
            raise AssertionError(f"{lane}: invalid JSON for {sql!r}: {exc}: {line}") from exc
        require(isinstance(payload, dict), f"{lane}: parser output is not an object")
        return payload
    raise AssertionError(f"{lane}: parser produced no JSON stdout for {sql!r}")


def run_parser(
    binary: Path,
    sql: str,
    lane: str,
    expected_returncode: int = 0,
) -> tuple[dict[str, object] | None, str]:
    require(binary.is_file(), f"{lane}: missing parser binary {binary}")
    completed = subprocess.run(
        [str(binary), sql],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=15,
    )
    require(
        completed.returncode == expected_returncode,
        f"{lane}: parser return code mismatch for {sql!r}: "
        f"rc={completed.returncode} expected={expected_returncode} stderr={completed.stderr}",
    )
    if expected_returncode == 0:
        return parse_json_stdout(completed.stdout, lane, sql), completed.stderr
    return None, completed.stderr


def verify_envelope(
    lane: str,
    row: dict[str, str],
    payload: dict[str, object],
    stderr: str,
    sql: str,
) -> None:
    disposition = row["disposition"]
    require(payload.get("envelope") == "SBLRExecutionEnvelope.v3",
            f"{lane}: missing SBLR envelope for {sql!r}")
    require(payload.get("dialect") == lane, f"{lane}: dialect mismatch for {sql!r}")
    require(payload.get("statement_family") == row["statement_family"],
            f"{lane}: statement family mismatch for {sql!r}")
    require(payload.get("operation_family") == row["operation_family"],
            f"{lane}: operation family mismatch for {sql!r}")
    require(payload.get("mapping_key") == row["mapping_key"],
            f"{lane}: mapping key mismatch for {sql!r}")
    require(payload.get("mapping_disposition") == DISPOSITION_TO_ENVELOPE[disposition],
            f"{lane}: mapping disposition mismatch for {sql!r}")
    require(payload.get("descriptor_resolution") == "uuid_required",
            f"{lane}: descriptor resolution drift for {sql!r}")
    require(payload.get("engine_authority") == "scratchbird",
            f"{lane}: engine authority drift for {sql!r}")
    require(payload.get("reference_engine_sql_executed") is False,
            f"{lane}: reference engine execution leaked into parser for {sql!r}")
    require(payload.get("real_reference_file_effects") is False,
            f"{lane}: reference file effects leaked into parser for {sql!r}")
    require(payload.get("sql_text_included") is False,
            f"{lane}: raw SQL text leaked into SBLR envelope for {sql!r}")

    if disposition in ROUTED_DISPOSITIONS:
        require(payload.get("sblr_operation"),
                f"{lane}: routed vector missing SBLR operation for {sql!r}")
        require(payload.get("engine_api_function"),
                f"{lane}: routed vector missing engine/parser API for {sql!r}")
        require(payload.get("fail_closed_refusal") is False,
                f"{lane}: routed vector was marked fail-closed for {sql!r}")
    if disposition == "ParserSupportUdr":
        require(payload.get("parser_support_udr_route") is True,
                f"{lane}: parser-support UDR route not set for {sql!r}")
    if disposition == "CatalogProjection":
        require(payload.get("catalog_projection_only") is True,
                f"{lane}: catalog projection route not set for {sql!r}")
    if disposition == "ScratchBirdLifecycleApi":
        require(payload.get("scratchbird_lifecycle_api") is True,
                f"{lane}: lifecycle route not set for {sql!r}")
    if disposition in FAIL_CLOSED_DISPOSITIONS:
        require(payload.get("fail_closed_refusal") is True,
                f"{lane}: fail-closed vector not marked as refusal for {sql!r}")
        require(row["diagnostic_code"] in stderr,
                f"{lane}: diagnostic {row['diagnostic_code']} missing for {sql!r}")


def verify_table_lane(repo_root: Path, parser_bin_root: Path, lane: str) -> dict[str, object]:
    source = repo_root / f"project/src/parsers/compatibility/{lane}/{lane}_dialect.cpp"
    binary = parser_binary(parser_bin_root, lane)
    rows = parse_patterns(source)
    failures: list[str] = []
    vectors = []

    for index, row in enumerate(rows, start=1):
        sql = sql_for_pattern(row)
        try:
            payload, stderr = run_parser(binary, sql, lane)
            verify_envelope(lane, row, payload, stderr, sql)
            vectors.append({
                "index": index,
                "operation_family": row["operation_family"],
                "disposition": row["disposition"],
                "sql": sql,
                "status": "passed",
            })
        except AssertionError as exc:
            failures.append(str(exc))
            vectors.append({
                "index": index,
                "operation_family": row["operation_family"],
                "disposition": row["disposition"],
                "sql": sql,
                "status": "failed",
                "failure": str(exc),
            })

    require(not failures, f"{lane}: command-vector failures: {failures[:8]}")
    return {
        "lane": lane,
        "parser_binary": str(binary),
        "vector_count": len(vectors),
        "routed_count": sum(1 for row in rows if row["disposition"] in ROUTED_DISPOSITIONS),
        "fail_closed_count": sum(1 for row in rows if row["disposition"] in FAIL_CLOSED_DISPOSITIONS),
        "vectors": vectors,
    }


def parse_diagnostic_stderr(stderr: str, lane: str, sql: str) -> dict[str, object]:
    for line in stderr.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            payload = json.loads(line)
        except json.JSONDecodeError as exc:
            raise AssertionError(f"{lane}: invalid diagnostic JSON for {sql!r}: {exc}: {line}") from exc
        require(isinstance(payload, dict), f"{lane}: diagnostic output is not an object")
        return payload
    raise AssertionError(f"{lane}: parser produced no diagnostic stderr for {sql!r}")


def verify_firebird_success(payload: dict[str, object], vector: dict[str, object]) -> None:
    sql = str(vector["sql"])
    require(payload.get("envelope") == "SBLRExecutionEnvelope.v3",
            f"firebird: missing SBLR envelope for {sql!r}")
    require(payload.get("dialect") == "firebird",
            f"firebird: dialect mismatch for {sql!r}")
    require(payload.get("operation_family") == vector["operation_family"],
            f"firebird: operation family mismatch for {sql!r}")
    require(payload.get("mapping_key") == vector["mapping_key"],
            f"firebird: mapping key mismatch for {sql!r}")
    require(payload.get("mapping_disposition") == vector["mapping_disposition"],
            f"firebird: mapping disposition mismatch for {sql!r}")
    require(payload.get("descriptor_resolution") == "uuid_required",
            f"firebird: descriptor resolution drift for {sql!r}")
    require(payload.get("engine_authority") == "scratchbird",
            f"firebird: engine authority drift for {sql!r}")
    require(payload.get("reference_engine_sql_executed") is False,
            f"firebird: reference execution leaked into parser for {sql!r}")
    require(payload.get("real_firebird_file_effects") is False,
            f"firebird: file effects leaked into parser for {sql!r}")
    require(payload.get("sql_text_included") is False,
            f"firebird: raw SQL text leaked into SBLR envelope for {sql!r}")
    for flag in (
        "scratchbird_lifecycle_api",
        "parser_support_udr_route",
        "exact_emulated_diagnostic",
    ):
        if flag in vector:
            require(payload.get(flag) is vector[flag],
                    f"firebird: {flag} mismatch for {sql!r}")


def verify_firebird_refusal(stderr: str, vector: dict[str, object]) -> None:
    sql = str(vector["sql"])
    payload = parse_diagnostic_stderr(stderr, "firebird", sql)
    diagnostics = payload.get("diagnostics")
    require(isinstance(diagnostics, list) and diagnostics,
            f"firebird: missing diagnostics for {sql!r}")
    diagnostic = diagnostics[0]
    require(isinstance(diagnostic, dict), f"firebird: malformed diagnostic for {sql!r}")
    require(diagnostic.get("code") == vector["diagnostic_code"],
            f"firebird: diagnostic code mismatch for {sql!r}")
    fields = diagnostic.get("fields")
    require(isinstance(fields, dict), f"firebird: missing diagnostic fields for {sql!r}")
    require(fields.get("operation_family") == vector["operation_family"],
            f"firebird: diagnostic operation family mismatch for {sql!r}")
    require(fields.get("mapping_key") == vector["mapping_key"],
            f"firebird: diagnostic mapping key mismatch for {sql!r}")
    require(fields.get("real_firebird_file_effects") == "false",
            f"firebird: diagnostic allowed file effects for {sql!r}")
    require(fields.get("reference_engine_sql_executed") == "false",
            f"firebird: diagnostic allowed reference execution for {sql!r}")


def verify_firebird_lane(parser_bin_root: Path) -> dict[str, object]:
    binary = parser_binary(parser_bin_root, "firebird")
    vectors = []
    failures: list[str] = []
    for index, vector in enumerate(FIREBIRD_VECTORS, start=1):
        expected_returncode = int(vector.get("expect_exit", 0))
        sql = str(vector["sql"])
        try:
            payload, stderr = run_parser(binary, sql, "firebird", expected_returncode)
            if expected_returncode == 0:
                require(payload is not None, f"firebird: missing success payload for {sql!r}")
                verify_firebird_success(payload, vector)
            else:
                verify_firebird_refusal(stderr, vector)
            vectors.append({
                "index": index,
                "operation_family": vector["operation_family"],
                "mapping_key": vector["mapping_key"],
                "sql": sql,
                "status": "passed",
            })
        except AssertionError as exc:
            failures.append(str(exc))
            vectors.append({
                "index": index,
                "operation_family": vector["operation_family"],
                "mapping_key": vector["mapping_key"],
                "sql": sql,
                "status": "failed",
                "failure": str(exc),
            })
    require(not failures, f"firebird: command-vector failures: {failures[:8]}")
    return {
        "lane": "firebird",
        "parser_binary": str(binary),
        "parser_kind": "custom_firebird_mapping",
        "vector_count": len(vectors),
        "routed_count": sum(
            1 for vector in FIREBIRD_VECTORS
            if vector.get("expect_exit", 0) == 0
            and not vector.get("exact_emulated_diagnostic", False)
        ),
        "fail_closed_count": sum(
            1 for vector in FIREBIRD_VECTORS
            if vector.get("expect_exit", 0) != 0
        ),
        "vectors": vectors,
    }


def verify_capability_reference_rows(manifest: dict[str, dict[str, str]]) -> list[dict[str, str]]:
    rows = []
    for lane in ("db2", "oracle", "sqlserver"):
        row = manifest[lane]
        require(row["profile_class"] == "capability_reference",
                f"{lane}: capability reference row became implementation row")
        require(row["parser_module"] == "forbidden",
                f"{lane}: capability reference row exposed parser module")
        rows.append({
            "lane": lane,
            "profile_class": row["profile_class"],
            "capability_reference_policy": row["capability_reference_policy"],
            "status": "blocked_from_public_parser_implementation",
        })
    return rows


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--parser-bin-root", required=True, type=Path)
    parser.add_argument("--evidence-file", required=True, type=Path)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    parser_bin_root = args.parser_bin_root.resolve()
    manifest = read_manifest(
        repo_root / "project/src/parsers/compatibility/CompatibilityProfileManifest.csv"
    )

    lane_results = [verify_firebird_lane(parser_bin_root)]
    lane_results.extend(
        verify_table_lane(repo_root, parser_bin_root, lane)
        for lane in TABLE_DRIVEN_LANES
    )
    capability_reference_rows = verify_capability_reference_rows(manifest)

    evidence = {
        "gate": "compatibility_reference_parser_full_lane_command_vectors",
        "status": "passed",
        "lane_count": len(lane_results),
        "vector_count": sum(result["vector_count"] for result in lane_results),
        "capability_reference_only_rows": capability_reference_rows,
        "engine_authority": "scratchbird",
        "transaction_authority": "mga",
        "sql_lowering_location": "parser_worker_outside_engine",
        "file_presence_is_completion": False,
        "lanes": lane_results,
    }
    args.evidence_file.parent.mkdir(parents=True, exist_ok=True)
    args.evidence_file.write_text(
        json.dumps(evidence, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(
        "compatibility_reference_parser_full_lane_command_vectors=passed "
        f"lanes={len(lane_results)} vectors={evidence['vector_count']}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"compatibility_reference_parser_full_lane_command_vectors: {exc}")
        raise SystemExit(1)
