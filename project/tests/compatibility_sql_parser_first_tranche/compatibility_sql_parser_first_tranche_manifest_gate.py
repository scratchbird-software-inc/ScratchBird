#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Compatibility SQL parser manifest and execution_plan proof gate.

This gate ties compatibility parser/UDR runtime probes to the row-level compatibility parser
manifests under project/tests.  It deliberately checks implementation behavior
and consumed manifests; file presence alone is not accepted as completion
evidence.
"""

from __future__ import annotations

import argparse
import csv
import json
import pathlib
import sys


DOCS_ROOT = "docs"
EXECUTION_PLAN_ROOT = DOCS_ROOT + "/" + "execution-plans"


DIALECTS = {
    "firebird": {
        "display": "FirebirdSQL",
        "execution_plan": EXECUTION_PLAN_ROOT + "/compatibility-parser-firebird-implementation-readiness",
        "udr_source": "project/src/udr/sbu_firebird_parser_support/sbu_firebird_parser_support.cpp",
        "parser_source": "project/src/parsers/compatibility/firebird/firebird_dialect.cpp",
        "package": "sbup_firebird",
        "logical_package": "firebird-v5_0",
        "routine_prefix": "sbu_firebird",
    },
    "mysql": {
        "display": "MySQL",
        "execution_plan": EXECUTION_PLAN_ROOT + "/compatibility-parser-mysql-implementation-readiness",
        "udr_source": "project/src/udr/sbu_mysql_parser_support/sbu_mysql_parser_support.cpp",
        "parser_source": "project/src/parsers/compatibility/mysql/mysql_dialect.cpp",
        "package": "sbup_mysql",
        "logical_package": "mysql-v8_4",
        "routine_prefix": "sbu_mysql",
    },
    "postgresql": {
        "display": "PostgreSQL",
        "execution_plan": EXECUTION_PLAN_ROOT + "/compatibility-parser-postgresql-implementation-readiness",
        "udr_source": "project/src/udr/sbu_postgresql_parser_support/sbu_postgresql_parser_support.cpp",
        "parser_source": "project/src/parsers/compatibility/postgresql/postgresql_dialect.cpp",
        "package": "sbup_postgresql",
        "logical_package": "postgresql-v16",
        "routine_prefix": "sbu_postgresql",
    },
    "sqlite": {
        "display": "SQLite",
        "execution_plan": "public_execution_plan",
        "udr_source": "project/src/udr/sbu_sqlite_parser_support/sbu_sqlite_parser_support.cpp",
        "parser_source": "project/src/parsers/compatibility/sqlite/sqlite_dialect.cpp",
        "package": "sbup_sqlite",
        "logical_package": "sqlite-v3_53",
        "routine_prefix": "sbu_sqlite",
    },
    "mariadb": {
        "display": "MariaDB",
        "execution_plan": "public_execution_plan",
        "udr_source": "project/src/udr/sbu_mariadb_parser_support/sbu_mariadb_parser_support.cpp",
        "parser_source": "project/src/parsers/compatibility/mariadb/mariadb_dialect.cpp",
        "package": "sbup_mariadb",
        "logical_package": "mariadb-v12_2",
        "routine_prefix": "sbu_mariadb",
    },
    "duckdb": {
        "display": "DuckDB",
        "execution_plan": "public_execution_plan",
        "udr_source": "project/src/udr/sbu_duckdb_parser_support/sbu_duckdb_parser_support.cpp",
        "parser_source": "project/src/parsers/compatibility/duckdb/duckdb_dialect.cpp",
        "package": "sbup_duckdb",
        "logical_package": "duckdb-v1_5",
        "routine_prefix": "sbu_duckdb",
    },
    "clickhouse": {
        "display": "ClickHouse",
        "execution_plan": "public_execution_plan",
        "udr_source": "project/src/udr/sbu_clickhouse_parser_support/sbu_clickhouse_parser_support.cpp",
        "parser_source": "project/src/parsers/compatibility/clickhouse/clickhouse_dialect.cpp",
        "package": "sbup_clickhouse",
        "logical_package": "clickhouse-v25_12",
        "routine_prefix": "sbu_clickhouse",
    },
    "tidb": {
        "display": "TiDB",
        "execution_plan": "public_execution_plan",
        "udr_source": "project/src/udr/sbu_tidb_parser_support/sbu_tidb_parser_support.cpp",
        "parser_source": "project/src/parsers/compatibility/tidb/tidb_dialect.cpp",
        "package": "sbup_tidb",
        "logical_package": "tidb-v8_5",
        "routine_prefix": "sbu_tidb",
    },
    "vitess": {
        "display": "Vitess",
        "execution_plan": "public_execution_plan",
        "udr_source": "project/src/udr/sbu_vitess_parser_support/sbu_vitess_parser_support.cpp",
        "parser_source": "project/src/parsers/compatibility/vitess/vitess_dialect.cpp",
        "package": "sbup_vitess",
        "logical_package": "vitess-v23_0",
        "routine_prefix": "sbu_vitess",
    },
    "cockroachdb": {
        "display": "CockroachDB",
        "execution_plan": "public_execution_plan",
        "udr_source": "project/src/udr/sbu_cockroachdb_parser_support/sbu_cockroachdb_parser_support.cpp",
        "parser_source": "project/src/parsers/compatibility/cockroachdb/cockroachdb_dialect.cpp",
        "package": "sbup_cockroachdb",
        "logical_package": "cockroachdb-v26_1",
        "routine_prefix": "sbu_cockroachdb",
    },
    "yugabytedb": {
        "display": "YugabyteDB",
        "execution_plan": "public_execution_plan",
        "udr_source": "project/src/udr/sbu_yugabytedb_parser_support/sbu_yugabytedb_parser_support.cpp",
        "parser_source": "project/src/parsers/compatibility/yugabytedb/yugabytedb_dialect.cpp",
        "package": "sbup_yugabytedb",
        "logical_package": "yugabytedb-v2025_2",
        "routine_prefix": "sbu_yugabytedb",
    },
    "cassandra": {
        "display": "Cassandra",
        "execution_plan": "public_execution_plan",
        "udr_source": "project/src/udr/sbu_cassandra_parser_support/sbu_cassandra_parser_support.cpp",
        "parser_source": "project/src/parsers/compatibility/cassandra/cassandra_dialect.cpp",
        "package": "sbup_cassandra",
        "logical_package": "cassandra-v5_0",
        "routine_prefix": "sbu_cassandra",
    },
    "mongodb": {
        "display": "MongoDB",
        "execution_plan": "public_execution_plan",
        "udr_source": "project/src/udr/sbu_mongodb_parser_support/sbu_mongodb_parser_support.cpp",
        "parser_source": "project/src/parsers/compatibility/mongodb/mongodb_dialect.cpp",
        "package": "sbup_mongodb",
        "logical_package": "mongodb-v8_2",
        "routine_prefix": "sbu_mongodb",
        "gate_prefix": "MONGO",
        "live_migration_gate": "MONGODB-GATE-LIVE-MIGRATION-METHOD",
    },
    "redis": {
        "display": "Redis",
        "execution_plan": "public_execution_plan",
        "udr_source": "project/src/udr/sbu_redis_parser_support/sbu_redis_parser_support.cpp",
        "parser_source": "project/src/parsers/compatibility/redis/redis_dialect.cpp",
        "package": "sbup_redis",
        "logical_package": "redis-v8_6",
        "routine_prefix": "sbu_redis",
    },
    "opensearch_sql_ppl": {
        "display": "OpenSearch SQL/PPL",
        "execution_plan": "public_execution_plan",
        "udr_source": "project/src/udr/sbu_opensearch_sql_ppl_parser_support/sbu_opensearch_sql_ppl_parser_support.cpp",
        "parser_source": "project/src/parsers/compatibility/opensearch_sql_ppl/opensearch_sql_ppl_dialect.cpp",
        "package": "sbup_opensearch_sql_ppl",
        "logical_package": "opensearch_sql_ppl-v3_6",
        "routine_prefix": "sbu_opensearch_sql_ppl",
        "gate_prefix": "OPENSEARCHSQLPPL",
        "live_migration_gate": "OPENSEARCH-SQL-PPL-GATE-LIVE-MIGRATION-METHOD",
    },
    "opensearch": {
        "display": "OpenSearch",
        "execution_plan": "public_execution_plan",
        "udr_source": "project/src/udr/sbu_opensearch_parser_support/sbu_opensearch_parser_support.cpp",
        "parser_source": "project/src/parsers/compatibility/opensearch/opensearch_dialect.cpp",
        "package": "sbup_opensearch",
        "logical_package": "opensearch-v3_6",
        "routine_prefix": "sbu_opensearch",
        "test_root": "project/tests/reference_regression/opensearch/rest_dsl",
        "gate_prefix": "OSREST",
        "live_migration_gate": "OPENSEARCH-REST-GATE-LIVE-MIGRATION-METHOD",
    },
    "neo4j": {
        "display": "Neo4j",
        "execution_plan": "public_execution_plan",
        "udr_source": "project/src/udr/sbu_neo4j_parser_support/sbu_neo4j_parser_support.cpp",
        "parser_source": "project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp",
        "package": "sbup_neo4j",
        "logical_package": "neo4j-v2026_04",
        "routine_prefix": "sbu_neo4j",
    },
    "influxdb": {
        "display": "InfluxDB",
        "execution_plan": "public_execution_plan",
        "udr_source": "project/src/udr/sbu_influxdb_parser_support/sbu_influxdb_parser_support.cpp",
        "parser_source": "project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp",
        "package": "sbup_influxdb",
        "logical_package": "influxdb-v3_9",
        "routine_prefix": "sbu_influxdb",
    },
    "milvus": {
        "display": "Milvus",
        "execution_plan": "public_execution_plan",
        "udr_source": "project/src/udr/sbu_milvus_parser_support/sbu_milvus_parser_support.cpp",
        "parser_source": "project/src/parsers/compatibility/milvus/milvus_dialect.cpp",
        "package": "sbup_milvus",
        "logical_package": "milvus-v2_6",
        "routine_prefix": "sbu_milvus",
    },
    "dolt": {
        "display": "Dolt",
        "execution_plan": "public_execution_plan",
        "udr_source": "project/src/udr/sbu_dolt_parser_support/sbu_dolt_parser_support.cpp",
        "parser_source": "project/src/parsers/compatibility/dolt/dolt_dialect.cpp",
        "package": "sbup_dolt",
        "logical_package": "dolt-v1_86",
        "routine_prefix": "sbu_dolt",
    },
    "apache_ignite": {
        "display": "Apache Ignite",
        "execution_plan": "public_execution_plan",
        "udr_source": "project/src/udr/sbu_apache_ignite_parser_support/sbu_apache_ignite_parser_support.cpp",
        "parser_source": "project/src/parsers/compatibility/apache_ignite/apache_ignite_dialect.cpp",
        "package": "sbup_apache_ignite",
        "logical_package": "apache_ignite-v2_17",
        "routine_prefix": "sbu_apache_ignite",
        "gate_prefix": "APACHEIGNITE",
        "live_migration_gate": "APACHE-IGNITE-GATE-LIVE-MIGRATION-METHOD",
    },
    "tikv": {
        "display": "TiKV",
        "execution_plan": "public_execution_plan",
        "udr_source": "project/src/udr/sbu_tikv_parser_support/sbu_tikv_parser_support.cpp",
        "parser_source": "project/src/parsers/compatibility/tikv/tikv_dialect.cpp",
        "package": "sbup_tikv",
        "logical_package": "tikv-v8_5",
        "routine_prefix": "sbu_tikv",
    },
    "foundationdb": {
        "display": "FoundationDB",
        "execution_plan": "public_execution_plan",
        "udr_source": "project/src/udr/sbu_foundationdb_parser_support/sbu_foundationdb_parser_support.cpp",
        "parser_source": "project/src/parsers/compatibility/foundationdb/foundationdb_dialect.cpp",
        "package": "sbup_foundationdb",
        "logical_package": "foundationdb-v7_3",
        "routine_prefix": "sbu_foundationdb",
        "gate_prefix": "FDB",
        "live_migration_gate": "FOUNDATIONDB-GATE-LIVE-MIGRATION-METHOD",
    },
    "immudb": {
        "display": "immudb",
        "execution_plan": "public_execution_plan",
        "udr_source": "project/src/udr/sbu_immudb_parser_support/sbu_immudb_parser_support.cpp",
        "parser_source": "project/src/parsers/compatibility/immudb/immudb_dialect.cpp",
        "package": "sbup_immudb",
        "logical_package": "immudb-v1_11",
        "routine_prefix": "sbu_immudb",
    },
    "xtdb": {
        "display": "XTDB",
        "execution_plan": "public_execution_plan",
        "udr_source": "project/src/udr/sbu_xtdb_parser_support/sbu_xtdb_parser_support.cpp",
        "parser_source": "project/src/parsers/compatibility/xtdb/xtdb_dialect.cpp",
        "package": "sbup_xtdb",
        "logical_package": "xtdb-v2_1",
        "routine_prefix": "sbu_xtdb",
    },
}

MANAGEMENT_OPERATIONS = (
    "describe_package",
    "validate_package",
    "get_capabilities",
    "classify_management_request",
    "render_management_diagnostic",
    "setup_pseudo_server",
    "alter_pseudo_server",
    "drop_pseudo_server",
    "validate_pseudo_server",
    "list_pseudo_servers",
    "setup_database",
    "alter_database",
    "drop_database",
    "rename_database",
    "attach_database",
    "detach_database",
    "validate_database",
    "create_catalog_projection",
    "refresh_catalog_projection",
    "validate_catalog_projection",
    "seed_catalog_rowsets",
    "export_catalog_projection",
    "install_domain_emulation",
    "refresh_domain_emulation",
    "validate_domain_emulation",
    "install_helper_routines",
    "validate_helper_routines",
    "normalize_login_identity",
    "validate_auth_evidence",
    "add_user",
    "alter_user",
    "drop_user",
    "map_external_identity",
    "validate_user_mapping",
    "create_role",
    "alter_role",
    "drop_role",
    "grant_role",
    "revoke_role",
    "grant_privilege",
    "revoke_privilege",
    "set_security_policy",
    "export_security_policy",
    "validate_security_policy",
    "set_session_option",
    "set_database_option",
    "set_server_option",
    "run_admin_command",
    "classify_external_authority_command",
    "render_status_report",
    "prepare_migration_context",
    "apply_migration_batch",
    "finalize_migration_context",
    "abort_migration_context",
    "start_replication_channel",
    "stop_replication_channel",
    "apply_replication_event",
    "get_replication_status",
    "validate_emulation_state",
    "get_operational_status",
    "collect_support_bundle",
    "export_metadata_snapshot",
    "import_metadata_snapshot",
    "retire_emulation_profile",
)

EXTERNAL_REFERENCE_SKIP_CODE = 77


def write_private_packet_skip_evidence(evidence_file: pathlib.Path) -> None:
    evidence_file.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "gate": "compatibility_sql_parser_first_tranche_manifest_gate",
        "status": "skipped",
        "skip_reason": "external_public_execution_plan_packet_not_installed",
        "required_packet": "public_execution_plan",
        "workplan_storage": "private workplan repository",
        "public_repo_policy": "workplans_are_not_tracked_in_scratchbird_public_repo",
    }
    evidence_file.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n",
                             encoding="utf-8")

MANAGEMENT_SURFACE_MAP = {
    "setup_database": "setup_database",
    "drop_database": "drop_database",
    "add_user": "add_user",
    "drop_user": "drop_user",
    "grant_privilege": "grant_privilege",
    "revoke_privilege": "revoke_privilege",
    "catalog_seed_projection": "seed_catalog_rowsets",
    "database_presentation": "validate_database",
    "migration_begin": "prepare_migration_context",
    "migration_cutover": "finalize_migration_context",
    "migration_rollback": "abort_migration_context",
    "replication_status": "get_replication_status",
    "support_bundle": "collect_support_bundle",
    "external_authority_refusal": "classify_external_authority_command",
}

WIRE_FAMILIES = {
    "connect_auth_startup",
    "prepare_bind_describe_execute_fetch_cancel_close",
    "error_warning_notice_status",
}

REQUIRED_GATE_SUFFIXES = {
    "001",
    "002",
    "003",
    "004",
    "005",
    "006",
    "007",
    "008",
    "009",
    "010",
    "011",
    "012",
    "019",
    "020",
    "021",
    "022",
    "023",
    "024",
    "025",
    "026",
    "027",
    "028",
    "029",
}


def read_csv(path: pathlib.Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None:
            raise AssertionError(f"{path}: missing CSV header")
        return [dict(row) for row in reader]


def require_file(repo_root: pathlib.Path, rel: str) -> pathlib.Path:
    path = repo_root / rel
    if not path.is_file():
        raise AssertionError(f"missing required file: {rel}")
    return path


def require_no_forbidden_completion_tokens(path: pathlib.Path) -> None:
    text = path.read_text(encoding="utf-8")
    forbidden = ("TODO", "STUB", "PLACEHOLDER", "NOT_IMPLEMENTED")
    found = [token for token in forbidden if token in text]
    if found:
        raise AssertionError(f"{path}: forbidden completion tokens present: {found}")


def require_execution_plan_gates(repo_root: pathlib.Path, dialect: str, spec: dict[str, str]) -> int:
    rel = spec["execution_plan"]
    path = require_file(repo_root, f"{rel}/ACCEPTANCE_GATES.csv")
    rows = read_csv(path)
    gate_ids = {row["gate_id"] for row in rows}
    prefix = spec.get("gate_prefix", dialect.upper())
    missing = [
        f"{prefix}-GATE-{suffix}"
        for suffix in sorted(REQUIRED_GATE_SUFFIXES)
        if f"{prefix}-GATE-{suffix}" not in gate_ids
    ]
    for required in (
        f"{prefix}-GATE-MGMT-ABI",
        spec.get("live_migration_gate", f"{prefix}-GATE-LIVE-MIGRATION-METHOD"),
    ):
      if required not in gate_ids:
          missing.append(required)
    if missing:
        raise AssertionError(f"{path}: missing required gates {missing}")
    return len(rows)


def require_management_manifest(repo_root: pathlib.Path, dialect: str) -> list[str]:
    test_root = DIALECTS[dialect].get("test_root", f"project/tests/reference_regression/{dialect}")
    path = require_file(
        repo_root,
        f"{test_root}/management_package_abi/management_package_abi_manifest.csv",
    )
    rows = read_csv(path)
    surfaces = [row["abi_surface"] for row in rows]
    missing_surfaces = sorted(set(MANAGEMENT_SURFACE_MAP) - set(surfaces))
    if missing_surfaces:
        raise AssertionError(f"{path}: missing ABI surfaces {missing_surfaces}")
    missing_operations = [
        operation for operation in MANAGEMENT_SURFACE_MAP.values()
        if operation not in MANAGEMENT_OPERATIONS
    ]
    if missing_operations:
        raise AssertionError(f"{path}: unmapped management operations {missing_operations}")
    for row in rows:
        if row["parser_rule"].find("cannot perform authority") == -1:
            raise AssertionError(f"{path}: parser authority rule weakened in {row}")
        if row["engine_rule"].find("engine authorizes") == -1:
            raise AssertionError(f"{path}: engine authorization rule missing in {row}")
        if "MGA_transaction_boundary" not in row["required_proof"]:
            raise AssertionError(f"{path}: MGA proof missing in {row}")
    return surfaces


def require_wire_manifest(repo_root: pathlib.Path, dialect: str) -> list[str]:
    test_root = DIALECTS[dialect].get("test_root", f"project/tests/reference_regression/{dialect}")
    path = require_file(
        repo_root,
        f"{test_root}/wire_transcripts/wire_transcript_manifest.csv",
    )
    rows = read_csv(path)
    families = {row["transcript_family"] for row in rows}
    missing = sorted(WIRE_FAMILIES - families)
    if missing:
        raise AssertionError(f"{path}: missing wire transcript families {missing}")
    for row in rows:
        if "normalize" not in row["normalization_rules"]:
            raise AssertionError(f"{path}: normalization rule missing in {row}")
        if not row["negative_cases"]:
            raise AssertionError(f"{path}: negative cases missing in {row}")
    return sorted(families)


def require_resource_manifest(repo_root: pathlib.Path, dialect: str) -> int:
    test_root = DIALECTS[dialect].get("test_root", f"project/tests/reference_regression/{dialect}")
    path = require_file(
        repo_root,
        f"{test_root}/resource_limits/resource_limit_manifest.csv",
    )
    rows = read_csv(path)
    required = {
        "input size recursion statement count bind count and batch count",
        "result streaming backpressure cancellation drain and timeout",
        "malformed frame command document and protocol abuse",
    }
    seen = {row["limit_surface"] for row in rows}
    missing = sorted(required - seen)
    if missing:
        raise AssertionError(f"{path}: missing resource limit surfaces {missing}")
    for row in rows:
        rule = row["mga_authority_rule"].lower()
        if (
            "mga" not in rule
            and "never commits rolls back or recovers" not in rule
            and "server transaction/session authority" not in rule
            and "parser-owned finality" not in rule
        ):
            raise AssertionError(f"{path}: MGA authority rule missing in {row}")
    return len(rows)


def require_enterprise_manifest(repo_root: pathlib.Path, dialect: str) -> int:
    test_root = DIALECTS[dialect].get("test_root", f"project/tests/reference_regression/{dialect}")
    path = require_file(
        repo_root,
        f"{test_root}/enterprise_completion/enterprise_completion_manifest.csv",
    )
    rows = read_csv(path)
    proof_families = {row["proof_family"] for row in rows}
    required = {
        "no_stubs_deferrals_or_minimal_completion",
        "generated_code_enterprise_completion",
        "integrated_end_to_end_proof",
        "crash_recovery_certification",
        "long_soak_certification",
        "security_review_closure",
        "operational_packaging_proof",
        "compatibility_guarantee",
        "independent_audit_closure",
    }
    missing = sorted(required - proof_families)
    if missing:
        raise AssertionError(f"{path}: missing enterprise proof families {missing}")
    for row in rows:
        if not row["required_project_tests_path"].startswith("project/tests/"):
            raise AssertionError(f"{path}: proof path outside project/tests in {row}")
    return len(rows)


def require_source_contract(repo_root: pathlib.Path, dialect: str, spec: dict[str, str]) -> None:
    parser_source = require_file(repo_root, spec["parser_source"])
    udr_source = require_file(repo_root, spec["udr_source"])
    require_no_forbidden_completion_tokens(parser_source)
    require_no_forbidden_completion_tokens(udr_source)
    udr_text = udr_source.read_text(encoding="utf-8")
    required_fragments = (
        f"{spec['routine_prefix']}_management_operation_inventory",
        f"{spec['routine_prefix']}_management_package_request",
        "management_abi_version",
        "parser_authority",
        "parser_selected_package_authority",
        "mga_transaction_authority",
        "scratchbird_engine",
        spec["logical_package"],
    )
    missing = [fragment for fragment in required_fragments if fragment not in udr_text]
    if missing:
        raise AssertionError(f"{udr_source}: missing management ABI fragments {missing}")
    for operation in MANAGEMENT_OPERATIONS:
        if operation not in udr_text:
            raise AssertionError(f"{udr_source}: missing mandatory operation {operation}")
    parser_text = parser_source.read_text(encoding="utf-8")
    report_fragment = (
        "FirebirdLifecycleMappingReportJson"
        if dialect == "firebird"
        else "SurfaceReportJson"
    )
    for fragment in ("PackageIdentityJson", report_fragment):
        if fragment not in parser_text:
            raise AssertionError(f"{parser_source}: missing parser profile fragment {fragment}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--evidence-file", required=True)
    args = parser.parse_args()
    repo_root = pathlib.Path(args.repo_root).resolve()
    evidence_file = pathlib.Path(args.evidence_file)
    if not (repo_root / "public_execution_plan").is_dir():
        write_private_packet_skip_evidence(evidence_file)
        print("compatibility_sql_parser_first_tranche_manifest_gate=skipped external_public_execution_plan_packet_not_installed")
        return EXTERNAL_REFERENCE_SKIP_CODE

    evidence: dict[str, object] = {
        "gate": "compatibility_sql_parser_first_tranche_manifest_gate",
        "status": "passed",
        "accepted_dialects": [],
        "management_operation_count": len(MANAGEMENT_OPERATIONS),
        "file_presence_is_completion": False,
        "runtime_behavior_required": True,
    }

    dialect_evidence = []
    for dialect, spec in DIALECTS.items():
        require_source_contract(repo_root, dialect, spec)
        gate_count = require_execution_plan_gates(repo_root, dialect, spec)
        management_surfaces = require_management_manifest(repo_root, dialect)
        wire_families = require_wire_manifest(repo_root, dialect)
        resource_rows = require_resource_manifest(repo_root, dialect)
        enterprise_rows = require_enterprise_manifest(repo_root, dialect)
        dialect_evidence.append(
            {
                "dialect": dialect,
                "package": spec["package"],
                "logical_package": spec["logical_package"],
                "execution_plan_gate_rows": gate_count,
                "management_manifest_surfaces": len(management_surfaces),
                "wire_transcript_families": wire_families,
                "resource_limit_rows": resource_rows,
                "enterprise_proof_rows": enterprise_rows,
                "runtime_gate": "compatibility_sql_parser_first_tranche",
            }
        )

    evidence["accepted_dialects"] = dialect_evidence
    evidence_file.parent.mkdir(parents=True, exist_ok=True)
    evidence_file.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n",
                             encoding="utf-8")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"compatibility_sql_parser_first_tranche_manifest_gate: {exc}", file=sys.stderr)
        raise SystemExit(1)
