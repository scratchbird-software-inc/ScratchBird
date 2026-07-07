#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate reference route backfill coverage without importing external grammar."""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path


EXPECTED_BACKFILL_ROWS = [
    ("SBSQL-REFERENCE-BACKFILL-001", "sqlite", "pragma_and_runtime_metadata"),
    ("SBSQL-REFERENCE-BACKFILL-002", "sqlite", "ddl_dml_query_and_maintenance"),
    ("SBSQL-REFERENCE-BACKFILL-003", "mariadb", "sequence_returning_locking_and_prepared"),
    ("SBSQL-REFERENCE-BACKFILL-004", "mariadb", "security_routine_replication_etl_and_maintenance"),
    ("SBSQL-REFERENCE-BACKFILL-005", "duckdb", "bulk_etl_security_routine_and_pragma"),
    ("SBSQL-REFERENCE-BACKFILL-006", "duckdb", "query_catalog_session_and_dml"),
    ("SBSQL-REFERENCE-BACKFILL-007", "clickhouse", "etl_security_connector_and_maintenance"),
    ("SBSQL-REFERENCE-BACKFILL-008", "clickhouse", "ddl_catalog_session_and_dml"),
    ("SBSQL-REFERENCE-BACKFILL-009", "tidb", "placement_resource_cdc_etl_and_admin"),
    ("SBSQL-REFERENCE-BACKFILL-010", "tidb", "ddl_dml_session_security"),
    ("SBSQL-REFERENCE-BACKFILL-011", "vitess", "topology_vschema_vreplication"),
    ("SBSQL-REFERENCE-BACKFILL-012", "vitess", "ddl_dml_session_security"),
    ("SBSQL-REFERENCE-BACKFILL-013", "cockroachdb", "changefeed_zone_cluster_catalog"),
    ("SBSQL-REFERENCE-BACKFILL-014", "cockroachdb", "ddl_dml_connector_security"),
    ("SBSQL-REFERENCE-BACKFILL-015", "yugabytedb", "ycql_tablet_tablegroup_cdc"),
    ("SBSQL-REFERENCE-BACKFILL-016", "yugabytedb", "connector_security_ddl_dml"),
    ("SBSQL-REFERENCE-BACKFILL-017", "cassandra", "keyspace_type_batch_session_security"),
    ("SBSQL-REFERENCE-BACKFILL-018", "cassandra", "ddl_dml_query_catalog"),
    ("SBSQL-REFERENCE-BACKFILL-019", "mongodb", "document_query_pipeline_index"),
    ("SBSQL-REFERENCE-BACKFILL-020", "mongodb", "document_dml_session_security_catalog_cdc"),
    ("SBSQL-REFERENCE-BACKFILL-021", "redis", "key_value_collections_streams_replication"),
    ("SBSQL-REFERENCE-BACKFILL-022", "redis", "script_admin_security_session"),
    ("SBSQL-REFERENCE-BACKFILL-023", "opensearch_sql_ppl", "search_sql_ppl_query_pipeline"),
    ("SBSQL-REFERENCE-BACKFILL-024", "opensearch_sql_ppl", "index_plugin_ml_catalog"),
    ("SBSQL-REFERENCE-BACKFILL-025", "opensearch_sql_ppl", "rest_dsl_search_request"),
    ("SBSQL-REFERENCE-BACKFILL-026", "opensearch", "rest_search_document_index"),
    ("SBSQL-REFERENCE-BACKFILL-027", "opensearch", "catalog_alias_pipeline_security"),
    ("SBSQL-REFERENCE-BACKFILL-028", "neo4j", "graph_query_and_mutation"),
    ("SBSQL-REFERENCE-BACKFILL-029", "neo4j", "schema_security_procedure_session_catalog"),
    ("SBSQL-REFERENCE-BACKFILL-030", "influxdb", "timeseries_query_write_flux"),
    ("SBSQL-REFERENCE-BACKFILL-031", "influxdb", "database_retention_continuous_query_catalog"),
    ("SBSQL-REFERENCE-BACKFILL-032", "milvus", "vector_collection_partition_index_load"),
    ("SBSQL-REFERENCE-BACKFILL-033", "milvus", "vector_dml_search_security"),
    ("SBSQL-REFERENCE-BACKFILL-034", "dolt", "version_control_catalog_and_remote_policy"),
    ("SBSQL-REFERENCE-BACKFILL-035", "dolt", "security_session_ddl_dml_query"),
    ("SBSQL-REFERENCE-BACKFILL-036", "apache_ignite", "cache_query_continuous_query_and_streaming"),
    ("SBSQL-REFERENCE-BACKFILL-037", "apache_ignite", "ddl_dml_optimizer_catalog_security_session"),
    ("SBSQL-REFERENCE-BACKFILL-038", "tikv", "raw_kv_txn_and_import"),
    ("SBSQL-REFERENCE-BACKFILL-039", "tikv", "coprocessor_catalog_and_region_policy"),
    ("SBSQL-REFERENCE-BACKFILL-040", "foundationdb", "kv_range_mutation_and_async"),
    ("SBSQL-REFERENCE-BACKFILL-041", "foundationdb", "directory_tuple_tenant_catalog"),
    ("SBSQL-REFERENCE-BACKFILL-042", "immudb", "verified_kv_reference_sorted_set_security_replication"),
    ("SBSQL-REFERENCE-BACKFILL-043", "immudb", "sql_catalog_session_and_dml"),
    ("SBSQL-REFERENCE-BACKFILL-044", "xtdb", "datalog_entity_bitemporal"),
    ("SBSQL-REFERENCE-BACKFILL-045", "xtdb", "sql_schema_catalog_and_modules"),
    ("SBSQL-REFERENCE-BACKFILL-046", "mysql", "replication_and_client_stream_etl"),
    ("SBSQL-REFERENCE-BACKFILL-047", "postgresql", "logical_replication_copy_and_connector"),
]

PROFILE_RENDERING_ALIAS = {
    "opensearch_sql_ppl": "opensearch",
}

ALIAS_TO_SBLR_FAMILY = {
    "query_select": "sblr.query.relational.v3",
    "dml_insert": "sblr.dml.insert.v3",
    "dml_update": "sblr.dml.update.v3",
    "dml_delete": "sblr.dml.delete.v3",
    "dml_merge_upsert": "sblr.dml.merge.v3",
    "ddl_create": "sblr.catalog.mutation.v3",
    "ddl_alter": "sblr.catalog.mutation.v3",
    "ddl_drop": "sblr.catalog.mutation.v3",
    "transaction_control": "sblr.transaction.control.v3",
    "session_settings": "sblr.session.setting.v3",
    "observability": "sblr.observability.inspect.v3",
    "bulk_io": "sblr.bulk.operation.v3",
    "function_call": "sblr.expression.runtime.v3",
}


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        return path.read_text(encoding="utf-8", errors="ignore")


def aliases_for_route(route_family: str) -> set[str]:
    text = route_family.lower()
    aliases: set[str] = set()
    if any(token in text for token in ("query", "select", "search", "find", "scan", "aggregate", "graph", "timeseries", "datalog", "coprocessor")):
        aliases.add("query_select")
    if any(token in text for token in ("insert", "write", "put", "set", "cache", "raw", "kv", "document", "mutation", "collections", "value")):
        aliases.add("dml_insert")
    if any(token in text for token in ("update", "modify", "set", "dml", "mutation", "datalog", "document", "vector")):
        aliases.add("dml_update")
    if any(token in text for token in ("delete", "remove", "clear", "del", "dml", "mutation", "document")):
        aliases.add("dml_delete")
    if any(token in text for token in ("merge", "upsert", "dml")):
        aliases.add("dml_merge_upsert")
    if any(token in text for token in ("ddl", "schema", "catalog", "index", "collection", "keyspace", "type", "partition", "table", "directory")):
        aliases.update({"ddl_create", "ddl_alter", "ddl_drop"})
    if any(token in text for token in ("txn", "transaction", "lock", "prepared", "consistency", "session")):
        aliases.add("transaction_control")
    if any(token in text for token in ("session", "security", "pragma", "setting", "config", "acl", "role", "auth", "policy")):
        aliases.add("session_settings")
    if any(token in text for token in ("catalog", "metadata", "maintenance", "admin", "optimizer", "explain", "runtime", "status", "topology", "zone", "placement", "region")):
        aliases.add("observability")
    if any(token in text for token in ("bulk", "copy", "load", "import", "export", "etl", "connector", "stream", "cdc", "changefeed", "subscription", "replication", "line_protocol", "client_file")):
        aliases.add("bulk_io")
    if any(token in text for token in ("function", "routine", "procedure", "script", "macro", "flux", "ml")):
        aliases.add("function_call")
    if not aliases:
        aliases.add("observability")
    return aliases


def validate_reference_backfill(repo: Path) -> list[str]:
    errors: list[str] = []
    artifact_root = repo / "project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/artifacts"
    coverage_rows = read_csv(artifact_root / "REFERENCE_ALIAS_COVERAGE_BACKLOG.csv")
    fixture_rows = read_csv(repo / "project/tests/sbsql_parser_worker/generated/reference_alias/REFERENCE_ALIAS_RENDERING_FIXTURES.csv")

    if len(EXPECTED_BACKFILL_ROWS) != 47:
        errors.append(f"expected 47 reference backfill rows, found {len(EXPECTED_BACKFILL_ROWS)}")

    fixture_aliases = {row["alias_kind"] for row in fixture_rows}
    if fixture_aliases != set(ALIAS_TO_SBLR_FAMILY):
        errors.append(
            "reference rendering fixture alias set mismatch: "
            f"missing={sorted(set(ALIAS_TO_SBLR_FAMILY) - fixture_aliases)} "
            f"extra={sorted(fixture_aliases - set(ALIAS_TO_SBLR_FAMILY))}"
        )

    by_profile_alias = {(row["reference"], row["alias_kind"]): row for row in coverage_rows}
    coverage_profiles = {row["reference"] for row in coverage_rows}
    covered_aliases: set[str] = set()
    covered_profiles: set[str] = set()
    seen_ids: set[str] = set()

    for row_id, profile, route_family in EXPECTED_BACKFILL_ROWS:
        if row_id in seen_ids:
            errors.append(f"duplicate reference backfill id {row_id}")
        seen_ids.add(row_id)
        rendering_profile = PROFILE_RENDERING_ALIAS.get(profile, profile)
        if rendering_profile not in coverage_profiles:
            errors.append(f"{row_id}: missing rendering profile {rendering_profile}")
            continue
        covered_profiles.add(profile)
        aliases = aliases_for_route(route_family)
        covered_aliases.update(aliases)
        for alias in aliases:
            coverage = by_profile_alias.get((rendering_profile, alias))
            if coverage is None:
                errors.append(f"{row_id}: {rendering_profile}:{alias} coverage row missing")
                continue
            expected_family = ALIAS_TO_SBLR_FAMILY[alias]
            if coverage.get("sblr_operation_family") != expected_family:
                errors.append(
                    f"{row_id}: {rendering_profile}:{alias} expected {expected_family}, "
                    f"found {coverage.get('sblr_operation_family')}"
                )
            if coverage.get("mapping_status") != "mapped_by_profile_or_refused_with_exact_diagnostic":
                errors.append(f"{row_id}: {rendering_profile}:{alias} mapping status not closed")
            if coverage.get("status") != "closed_by_reference_alias_rendering_gate":
                errors.append(f"{row_id}: {rendering_profile}:{alias} status not closed by rendering gate")
            if coverage.get("engine_owned_behavior") != "UUID authority, descriptor authority, security, transaction/MGA state, storage effects, metrics, and SBLR execution":
                errors.append(f"{row_id}: {rendering_profile}:{alias} engine authority boundary changed")
            if coverage.get("final_acceptance_rule") != "maps_to_native_surface_or_exact_policy_refusal_with_reference_rendering_test":
                errors.append(f"{row_id}: {rendering_profile}:{alias} final acceptance rule changed")

    if covered_aliases != set(ALIAS_TO_SBLR_FAMILY):
        errors.append(
            "backfill rows do not consume every reference alias class: "
            f"missing={sorted(set(ALIAS_TO_SBLR_FAMILY) - covered_aliases)}"
        )

    expected_profiles = {profile for _, profile, _ in EXPECTED_BACKFILL_ROWS}
    if covered_profiles != expected_profiles:
        errors.append(
            "backfill profile set mismatch: "
            f"missing={sorted(expected_profiles - covered_profiles)} "
            f"extra={sorted(covered_profiles - expected_profiles)}"
        )

    report = read_text(artifact_root / "REFERENCE_ALIAS_RENDERING_REPORT.md")
    for token in ("Status: complete", "312", "Reference Profiles", "Alias kinds", "engine authority"):
        if token not in report:
            errors.append(f"reference alias rendering report missing {token!r}")

    public_snapshot = read_text(repo / "public_contract_snapshot")
    if "reference_command_function_backfill:" not in public_snapshot:
        errors.append("public contract snapshot missing reference_command_function_backfill")
    if "reference_alias_preserved" not in public_snapshot:
        errors.append("public contract snapshot missing reference_alias_preserved evidence")

    runtime_text = "\n".join(
        read_text(path)
        for path in [
            repo / "project/src/parsers/shared/sbsql_v3_binding/sbsql_v3_binding_catalog.cpp",
            repo / "project/src/parsers/shared/sbsql_v3_ast/sbsql_v3_ast_catalog.cpp",
            repo / "project/src/parsers/shared/sbsql_v3_sblr/sbsql_v3_sblr_catalog.cpp",
        ]
    )
    if "sbsql.reference_command_function_backfill" not in runtime_text:
        errors.append("shared parser catalogs missing reference command/function backfill family")
    for banned in ("sbsql." + "do" + "nor_command_function_backfill", "SBLR_" + "DO" + "NOR"):
        if banned in runtime_text:
            errors.append(f"shared parser catalogs still expose banned compatibility token {banned}")

    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()

    repo = Path(args.repo_root).resolve()
    try:
        errors = validate_reference_backfill(repo)
    except Exception as exc:  # noqa: BLE001 - gate output should be explicit.
        print("sbsql_reference_route_backfill_conformance_gate=failed", file=sys.stderr)
        print(str(exc), file=sys.stderr)
        return 2

    if errors:
        print("sbsql_reference_route_backfill_conformance_gate=failed", file=sys.stderr)
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    print("sbsql_reference_route_backfill_conformance_gate=passed")
    print(f"reference_route_families={len(EXPECTED_BACKFILL_ROWS)}")
    print(f"reference_profiles={len({profile for _, profile, _ in EXPECTED_BACKFILL_ROWS})}")
    print(f"alias_classes={len(ALIAS_TO_SBLR_FAMILY)}")
    print("grammar_import_policy=native_sbsql_or_exact_refusal")
    print("engine_authority=SBLR_UUID_MGA_only")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
