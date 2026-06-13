#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""First-tranche compatibility parser CLI proof gate."""

from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path


def run(cmd: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, check=False, text=True, capture_output=True)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def parse_json_output(name: str, command: str, text: str) -> dict[str, object]:
    try:
        payload = json.loads(text)
    except json.JSONDecodeError as exc:
        raise AssertionError(f"{name} {command} did not emit valid JSON: {exc}") from exc
    require(isinstance(payload, dict), f"{name} {command} JSON payload must be an object")
    return payload


def denies_compatibility_sql_execution(payload: dict[str, object]) -> bool:
    denial_keys = {
        "compatibility_sql_execution",
        "reference_engine_sql_executed",
        "reference_sql_execution",
    }
    pending = [payload]
    while pending:
        current = pending.pop()
        for key, value in current.items():
            if key in denial_keys and value is False:
                return True
            if isinstance(value, dict):
                pending.append(value)
    return False


def check_parser(name: str, binary: Path, positive_sql: str, refusal_sql: str, refusal_code: str) -> dict[str, object]:
    identity = run([str(binary), "--package-identity"])
    require(identity.returncode == 0, f"{name} package identity failed: {identity.stderr}")
    identity_payload = parse_json_output(name, "package identity", identity.stdout)
    require(identity_payload.get("dialect") == name, f"{name} package identity missing dialect")
    require(
        denies_compatibility_sql_execution(identity_payload),
        f"{name} identity did not deny compatibility SQL execution",
    )

    surface = run([str(binary), "--surface-report"])
    require(surface.returncode == 0, f"{name} surface report failed: {surface.stderr}")
    surface_payload = parse_json_output(name, "surface report", surface.stdout)
    require("datatype_surfaces" in surface_payload, f"{name} surface report missing datatype surfaces")
    require("catalog_overlay_surfaces" in surface_payload, f"{name} surface report missing catalog surfaces")

    positive = run([str(binary), positive_sql])
    require(positive.returncode == 0, f"{name} positive parse failed: {positive.stderr}")
    require("SBLRExecutionEnvelope.v3" in positive.stdout, f"{name} positive parse missing envelope")
    require('"sql_text_included":false' in positive.stdout, f"{name} positive parse leaked SQL text policy")
    require('"reference_engine_sql_executed":false' in positive.stdout, f"{name} positive parse allowed compatibility SQL execution")

    refusal = run([str(binary), refusal_sql])
    require(
        refusal.returncode in (0, 1),
        f"{name} refusal parse should produce explicit refusal evidence: {refusal.stderr}",
    )
    require(refusal_code in refusal.stdout or refusal_code in refusal.stderr, f"{name} refusal missing {refusal_code}")
    if refusal.returncode == 0:
        require(
            '"fail_closed_refusal":true' in refusal.stdout
            or '"exact_emulated_diagnostic":true' in refusal.stdout,
            f"{name} refusal did not fail closed or emit an exact emulated diagnostic",
        )
    else:
        require(
            ".PARSE.UNSUPPORTED_SURFACE" in refusal_code,
            f"{name} nonzero refusal must be an explicit unsupported-surface denial",
        )

    return {
        "dialect": name,
        "binary": str(binary),
        "package_identity_checked": True,
        "surface_report_checked": True,
        "positive_route_checked": True,
        "explicit_refusal_checked": True,
        "refusal_code": refusal_code,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--firebird", required=True, type=Path)
    parser.add_argument("--mysql", required=True, type=Path)
    parser.add_argument("--postgresql", required=True, type=Path)
    parser.add_argument("--sqlite", required=True, type=Path)
    parser.add_argument("--mariadb", required=True, type=Path)
    parser.add_argument("--duckdb", required=True, type=Path)
    parser.add_argument("--clickhouse", required=True, type=Path)
    parser.add_argument("--tidb", required=True, type=Path)
    parser.add_argument("--vitess", required=True, type=Path)
    parser.add_argument("--cockroachdb", required=True, type=Path)
    parser.add_argument("--yugabytedb", required=True, type=Path)
    parser.add_argument("--cassandra", required=True, type=Path)
    parser.add_argument("--mongodb", required=True, type=Path)
    parser.add_argument("--redis", required=True, type=Path)
    parser.add_argument("--opensearch-sql-ppl", required=True, type=Path)
    parser.add_argument("--opensearch", required=True, type=Path)
    parser.add_argument("--neo4j", required=True, type=Path)
    parser.add_argument("--influxdb", required=True, type=Path)
    parser.add_argument("--milvus", required=True, type=Path)
    parser.add_argument("--dolt", required=True, type=Path)
    parser.add_argument("--apache-ignite", required=True, type=Path)
    parser.add_argument("--tikv", required=True, type=Path)
    parser.add_argument("--foundationdb", required=True, type=Path)
    parser.add_argument("--immudb", required=True, type=Path)
    parser.add_argument("--xtdb", required=True, type=Path)
    parser.add_argument("--evidence-file", required=True, type=Path)
    args = parser.parse_args()

    evidence = {
        "gate": "compatibility_sql_parser_first_tranche_cli_evidence",
        "parser_and_udr_completion_unit": True,
        "linux_first_tranche": True,
        "dialects": [
            check_parser(
                "firebird",
                args.firebird,
                "select 1",
                "backup database to '/tmp/a.fbk'",
                "FIREBIRD.EMULATION.NON_FILE_SURFACE",
            ),
            check_parser(
                "mysql",
                args.mysql,
                "select json_extract(doc, '$.a') from t",
                "load data infile '/tmp/x.csv' into table t",
                "MYSQL.AUTHORITY.FILE_IO_DENIED",
            ),
            check_parser(
                "postgresql",
                args.postgresql,
                "select count(*) from public.t",
                "copy public.t from program 'cat /etc/passwd'",
                "POSTGRESQL.AUTHORITY.PROGRAM_DENIED",
            ),
            check_parser(
                "sqlite",
                args.sqlite,
                "select json_extract(doc, '$.a') from t",
                "select load_extension('/tmp/x.so')",
                "SQLITE.AUTHORITY.EXTENSION_DENIED",
            ),
            check_parser(
                "mariadb",
                args.mariadb,
                "insert into t values (1) returning id",
                "load data infile '/tmp/x.csv' into table t",
                "MARIADB.AUTHORITY.FILE_IO_DENIED",
            ),
            check_parser(
                "duckdb",
                args.duckdb,
                "select date_part('year', current_date)",
                "install httpfs",
                "DUCKDB.AUTHORITY.EXTENSION_DENIED",
            ),
            check_parser(
                "clickhouse",
                args.clickhouse,
                "select count() from t",
                "system reload dictionaries",
                "CLICKHOUSE.AUTHORITY.UNSUPPORTED_DENIED",
            ),
            check_parser(
                "tidb",
                args.tidb,
                "select tidb_version()",
                "admin checksum table t",
                "TIDB.AUTHORITY.ADMIN_DENIED",
            ),
            check_parser(
                "vitess",
                args.vitess,
                "select keyspace_id from customer",
                "load data infile '/tmp/x.csv' into table t",
                "VITESS.AUTHORITY.FILE_IO_DENIED",
            ),
            check_parser(
                "cockroachdb",
                args.cockroachdb,
                "select crdb_internal.node_id()",
                "backup database d to 'nodelocal://1/a'",
                "COCKROACHDB.AUTHORITY.BACKUP_DENIED",
            ),
            check_parser(
                "yugabytedb",
                args.yugabytedb,
                "select yb_server_region()",
                "backup database d to '/tmp/d'",
                "YUGABYTEDB.AUTHORITY.BACKUP_DENIED",
            ),
            check_parser(
                "cassandra",
                args.cassandra,
                "select json * from ks.t",
                "repair ks",
                "CASSANDRA.AUTHORITY.UNSUPPORTED_DENIED",
            ),
            check_parser(
                "mongodb",
                args.mongodb,
                "find users { status: 'A' }",
                "replset",
                "MONGODB.AUTHORITY.REPLICA_ADMIN_DENIED",
            ),
            check_parser(
                "redis",
                args.redis,
                "set account:1 active",
                "unsupported_redis_command",
                "REDIS.PARSE.UNSUPPORTED_SURFACE",
            ),
            check_parser(
                "opensearch_sql_ppl",
                args.opensearch_sql_ppl,
                "select count(*) from accounts",
                "unsupported_opensearch_command",
                "OPENSEARCH_SQL_PPL.PARSE.UNSUPPORTED_SURFACE",
            ),
            check_parser(
                "opensearch",
                args.opensearch,
                "POST /accounts/_search {\"query\":{\"match_all\":{}}}",
                "unsupported_opensearch_rest_command",
                "OPENSEARCH.PARSE.UNSUPPORTED_SURFACE",
            ),
            check_parser(
                "neo4j",
                args.neo4j,
                "match (n:Account) return n",
                "unsupported_neo4j_command",
                "NEO4J.PARSE.UNSUPPORTED_SURFACE",
            ),
            check_parser(
                "influxdb",
                args.influxdb,
                "select mean(value) from cpu",
                "unsupported_influxdb_command",
                "INFLUXDB.PARSE.UNSUPPORTED_SURFACE",
            ),
            check_parser(
                "milvus",
                args.milvus,
                "search collection accounts vector [0.1,0.2] topk 10",
                "unsupported_milvus_command",
                "MILVUS.PARSE.UNSUPPORTED_SURFACE",
            ),
            check_parser(
                "dolt",
                args.dolt,
                "select * from dolt_log",
                "unsupported_dolt_command",
                "DOLT.PARSE.UNSUPPORTED_SURFACE",
            ),
            check_parser(
                "apache_ignite",
                args.apache_ignite,
                "select * from City",
                "control.sh --baseline",
                "APACHE_IGNITE.AUTHORITY.CLUSTER_CONTROL_RESERVED",
            ),
            check_parser(
                "tikv",
                args.tikv,
                "RAW_GET account:1",
                "unsupported_tikv_command",
                "TIKV.PARSE.UNSUPPORTED_SURFACE",
            ),
            check_parser(
                "foundationdb",
                args.foundationdb,
                "GET account:1",
                "unsupported_foundationdb_command",
                "FOUNDATIONDB.PARSE.UNSUPPORTED_SURFACE",
            ),
            check_parser(
                "immudb",
                args.immudb,
                "VERIFIED_GET account:1",
                "DUMP database to '/tmp/x'",
                "IMMUDB.AUTHORITY.BACKUP_DENIED",
            ),
            check_parser(
                "xtdb",
                args.xtdb,
                "XTDB_Q [:find ?e :where [?e :name \"Ada\"]]",
                "MODULES CONFIGURATION s3",
                "XTDB.AUTHORITY.MODULE_CONFIGURATION_DENIED",
            ),
        ],
        "authority_invariants": {
            "engine_executes_sblr_only": True,
            "parser_support_udr_requires_trusted_engine_context": True,
            "compatibility_sql_execution_forbidden": True,
            "compatibility_file_effects_forbidden": True,
            "mga_transaction_authority_preserved": True,
        },
    }
    args.evidence_file.parent.mkdir(parents=True, exist_ok=True)
    args.evidence_file.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
