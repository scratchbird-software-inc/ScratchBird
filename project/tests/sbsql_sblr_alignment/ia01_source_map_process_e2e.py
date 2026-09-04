#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

"""CSC-TEST-002337: real standalone SOURCE_MAP process proof."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

from ia01_package_process_e2e import ProofError, allocate_work, seed_database, stop, wait_unix


STATIC_EXECUTOR_EVIDENCE_REFUSALS = {
    "ddl-create-temporary-table": (
        "CSC-TEST-002661", "DDL_CREATE_TEMPORARY_TABLE",
        "engine.op.ddl_create_temporary_table",
        "SBLR_DDL_CREATE_TEMPORARY_TABLE", 1612,
    ),
    "ddl-drop-temporary-table": (
        "CSC-TEST-002665", "DDL_DROP_TEMPORARY_TABLE",
        "engine.op.ddl_drop_temporary_table",
        "SBLR_DDL_DROP_TEMPORARY_TABLE", 1613,
    ),
    "show-wait-events": (
        "CSC-TEST-002702", "READ_METRICS", "engine.op.read_metrics",
        "SBLR_READ_METRICS", 3073,
    ),
    "ddl-create-or-replace-srs": (
        "CSC-TEST-002673", "DDL_CREATE_OR_REPLACE_SRS",
        "engine.op.ddl_create_or_replace_srs",
        "SBLR_DDL_CREATE_OR_REPLACE_SRS", 1615,
    ),
    "ddl-drop-srs": (
        "CSC-TEST-002677", "DDL_DROP_SRS", "engine.op.ddl_drop_srs",
        "SBLR_DDL_DROP_SRS", 1616,
    ),
    "ddl-create-rewrite-rule": (
        "CSC-TEST-002681", "DDL_CREATE_REWRITE_RULE",
        "engine.op.ddl_create_rewrite_rule", "SBLR_DDL_CREATE_REWRITE_RULE",
        1617,
    ),
    "ddl-alter-rewrite-rule": (
        "CSC-TEST-002685", "DDL_ALTER_REWRITE_RULE",
        "engine.op.ddl_alter_rewrite_rule", "SBLR_DDL_ALTER_REWRITE_RULE",
        1618,
    ),
    "ddl-drop-rewrite-rule": (
        "CSC-TEST-002689", "DDL_DROP_REWRITE_RULE",
        "engine.op.ddl_drop_rewrite_rule", "SBLR_DDL_DROP_REWRITE_RULE",
        1619,
    ),
    "ddl-validate-constraint": (
        "CSC-TEST-002693", "DDL_VALIDATE_CONSTRAINT",
        "engine.op.ddl_validate_constraint", "SBLR_DDL_VALIDATE_CONSTRAINT",
        1620,
    ),
    "ddl-create-macro": (
        "CSC-TEST-002745", "DDL_CREATE_MACRO",
        "engine.op.ddl_create_macro", "SBLR_DDL_CREATE_MACRO", 1633,
    ),
    "security-create-user": (
        "CSC-TEST-002965", "SECURITY_CREATE_USER",
        "engine.op.sec_create_user", "SBLR_SEC_CREATE_USER", 1792,
    ),
    "ddl-alter-aggregate": (
        "CSC-TEST-002717", "DDL_ALTER_AGGREGATE",
        "engine.op.ddl_alter_aggregate", "SBLR_DDL_ALTER_AGGREGATE", 1626,
    ),
    "ddl-drop-aggregate": (
        "CSC-TEST-002721", "DDL_DROP_AGGREGATE",
        "engine.op.ddl_drop_aggregate", "SBLR_DDL_DROP_AGGREGATE", 1627,
    ),
    "ddl-purge-system-history": (
        "CSC-TEST-002725", "DDL_PURGE_SYSTEM_HISTORY",
        "engine.op.ddl_purge_system_history", "SBLR_DDL_PURGE_SYSTEM_HISTORY",
        1628,
    ),
    "ddl-set-index-optimizer-eligibility": (
        "CSC-TEST-002729", "DDL_SET_INDEX_OPTIMIZER_ELIGIBILITY",
        "engine.op.ddl_set_index_optimizer_eligibility",
        "SBLR_DDL_SET_INDEX_OPTIMIZER_ELIGIBILITY", 1629,
    ),
    "ddl-set-table-type-enforcement": (
        "CSC-TEST-002733", "DDL_SET_TABLE_TYPE_ENFORCEMENT",
        "engine.op.ddl_set_table_type_enforcement",
        "SBLR_DDL_SET_TABLE_TYPE_ENFORCEMENT", 1630,
    ),
    "database-serialize-logical-snapshot": (
        "CSC-TEST-002737", "DATABASE_SERIALIZE_LOGICAL_SNAPSHOT",
        "engine.op.database_serialize_logical_snapshot",
        "SBLR_DATABASE_SERIALIZE_LOGICAL_SNAPSHOT", 1631,
    ),
    "database-deserialize-logical-snapshot": (
        "CSC-TEST-002741", "DATABASE_DESERIALIZE_LOGICAL_SNAPSHOT",
        "engine.op.database_deserialize_logical_snapshot",
        "SBLR_DATABASE_DESERIALIZE_LOGICAL_SNAPSHOT", 1632,
    ),
    "ddl-drop-macro": (
        "CSC-TEST-002749", "DDL_DROP_MACRO",
        "engine.op.ddl_drop_macro", "SBLR_DDL_DROP_MACRO", 1634,
    ),
    "ddl-drop-package": (
        "CSC-TEST-002893", "DDL_DROP_PACKAGE",
        "engine.op.ddl_drop_package", "SBLR_DDL_DROP_PACKAGE", 1562,
    ),
    "admin-register-external-relation-resolver": (
        "CSC-TEST-002753", "ADMIN_REGISTER_EXTERNAL_RELATION_RESOLVER",
        "engine.op.admin_register_external_relation_resolver",
        "SBLR_ADMIN_REGISTER_EXTERNAL_RELATION_RESOLVER", 1635,
    ),
    "admin-unregister-external-relation-resolver": (
        "CSC-TEST-002757", "ADMIN_UNREGISTER_EXTERNAL_RELATION_RESOLVER",
        "engine.op.admin_unregister_external_relation_resolver",
        "SBLR_ADMIN_UNREGISTER_EXTERNAL_RELATION_RESOLVER", 1636,
    ),
    "security-alter-privilege-template": (
        "CSC-TEST-002701", "SECURITY_ALTER_PRIVILEGE_TEMPLATE",
        "engine.op.security_alter_privilege_template",
        "SBLR_SECURITY_ALTER_PRIVILEGE_TEMPLATE", 1622,
    ),
    "security-drop-privilege-template": (
        "CSC-TEST-002705", "SECURITY_DROP_PRIVILEGE_TEMPLATE",
        "engine.op.security_drop_privilege_template",
        "SBLR_SECURITY_DROP_PRIVILEGE_TEMPLATE", 1623,
    ),
    "database-create-template-clone": (
        "CSC-TEST-002709", "DATABASE_CREATE_TEMPLATE_CLONE",
        "engine.op.database_create_template_clone",
        "SBLR_DATABASE_CREATE_TEMPLATE_CLONE", 1624,
    ),
    "ddl-create-aggregate": (
        "CSC-TEST-002713", "DDL_CREATE_AGGREGATE",
        "engine.op.ddl_create_aggregate", "SBLR_DDL_CREATE_AGGREGATE",
        1625,
    ),
    "ddl-create-dictionary": (
        "CSC-TEST-002761", "DDL_CREATE_DICTIONARY",
        "engine.op.ddl_create_dictionary", "SBLR_DDL_CREATE_DICTIONARY",
        1637,
    ),
    "ddl-drop-dictionary": (
        "CSC-TEST-002769", "DDL_DROP_DICTIONARY",
        "engine.op.ddl_drop_dictionary", "SBLR_DDL_DROP_DICTIONARY", 1638,
    ),
    "ddl-alter-dictionary": (
        "CSC-TEST-002765", "DDL_ALTER_DICTIONARY",
        "engine.op.ddl_alter_dictionary", "SBLR_DDL_ALTER_DICTIONARY",
        1639,
    ),
    "ddl-create-continuous-view": (
        "CSC-TEST-002773", "DDL_CREATE_CONTINUOUS_VIEW",
        "engine.op.ddl_create_continuous_view",
        "SBLR_DDL_CREATE_CONTINUOUS_VIEW", 1640,
    ),
    "ddl-alter-continuous-view": (
        "CSC-TEST-002777", "DDL_ALTER_CONTINUOUS_VIEW",
        "engine.op.ddl_alter_continuous_view",
        "SBLR_DDL_ALTER_CONTINUOUS_VIEW", 1641,
    ),
    "ddl-drop-continuous-view": (
        "CSC-TEST-002781", "DDL_DROP_CONTINUOUS_VIEW",
        "engine.op.ddl_drop_continuous_view",
        "SBLR_DDL_DROP_CONTINUOUS_VIEW", 1642,
    ),
    "dml-async-insert-submit": (
        "CSC-TEST-002785", "DML_ASYNC_INSERT_SUBMIT",
        "engine.op.dml_async_insert_submit",
        "SBLR_DML_ASYNC_INSERT_SUBMIT", 1643,
    ),
    "dml-async-insert-status": (
        "CSC-TEST-002789", "DML_ASYNC_INSERT_STATUS",
        "engine.op.dml_async_insert_status",
        "SBLR_DML_ASYNC_INSERT_STATUS", 1644,
    ),
    "dml-async-insert-cancel": (
        "CSC-TEST-002793", "DML_ASYNC_INSERT_CANCEL",
        "engine.op.dml_async_insert_cancel",
        "SBLR_DML_ASYNC_INSERT_CANCEL", 1645,
    ),
    "dml-conditional-mutate": (
        "CSC-TEST-002797", "DML_CONDITIONAL_MUTATE",
        "engine.op.dml_conditional_mutate",
        "SBLR_DML_CONDITIONAL_MUTATE", 1646,
    ),
    "dml-timeseries-schema-write": (
        "CSC-TEST-002805", "DML_TIMESERIES_SCHEMA_WRITE",
        "engine.op.dml_timeseries_schema_write",
        "SBLR_DML_TIMESERIES_SCHEMA_WRITE", 1648,
    ),
    "ddl-timeseries-series-cardinality-policy": (
        "CSC-TEST-002809", "DDL_SET_TIMESERIES_SERIES_CARDINALITY_POLICY",
        "engine.op.ddl_set_timeseries_series_cardinality_policy",
        "SBLR_DDL_SET_TIMESERIES_SERIES_CARDINALITY_POLICY", 1649,
    ),
    "ddl-create-timeseries-value-cache": (
        "CSC-TEST-002813", "DDL_CREATE_TIMESERIES_VALUE_CACHE",
        "engine.op.ddl_create_timeseries_value_cache",
        "SBLR_DDL_CREATE_TIMESERIES_VALUE_CACHE", 1650,
    ),
    "ddl-alter-timeseries-value-cache": (
        "CSC-TEST-002817", "DDL_ALTER_TIMESERIES_VALUE_CACHE",
        "engine.op.ddl_alter_timeseries_value_cache",
        "SBLR_DDL_ALTER_TIMESERIES_VALUE_CACHE", 1651,
    ),
    "ddl-create-synonym": (
        "CSC-TEST-002941", "DDL_CREATE_SYNONYM",
        "engine.op.ddl_create_synonym", "SBLR_DDL_CREATE_SYNONYM", 1574,
    ),
    "ddl-create-foreign-table": (
        "CSC-TEST-002949", "DDL_CREATE_FOREIGN_TABLE",
        "engine.op.ddl_create_foreign_table",
        "SBLR_DDL_CREATE_FOREIGN_TABLE", 1576,
    ),
    "ddl-create-fdw": (
        "CSC-TEST-002957", "DDL_CREATE_FDW", "engine.op.ddl_create_fdw",
        "SBLR_DDL_CREATE_FDW", 1578,
    ),
    "ddl-drop-fdw": (
        "CSC-TEST-002961", "DDL_DROP_FDW", "engine.op.ddl_drop_fdw",
        "SBLR_DDL_DROP_FDW", 1579,
    ),
    "ddl-drop-foreign-table": (
        "CSC-TEST-002953", "DDL_DROP_FOREIGN_TABLE",
        "engine.op.ddl_drop_foreign_table", "SBLR_DDL_DROP_FOREIGN_TABLE",
        1577,
    ),
}

# Most static evidence refusals are rejected while the package members are
# preflighted.  A small number have an operation-specific public-ABI fence and
# therefore publish the more precise audit key below.  Keep these exact rather
# than accepting an arbitrary non-empty audit field.
STATIC_EXECUTOR_EVIDENCE_AUDIT_KEYS = {
    "security-create-user": "sblr.security_create_user.executor_unavailable",
}

PRE_CONTEXT_COMMAND_REFUSALS = {
    "stmt-prepare": (
        "CSC-TEST-003573", "STMT_PREPARE",
        (
            "engine.op.stmt_prepare",
            "statement.prepare",
            "session.prepare_statement",
            "SBLR_STMT_PREPARE",
            "SBLR_SESSION_PREPARE_STATEMENT",
            "opcode_code=4608",
            "stmt_prepare_descriptor",
            "stmt_prepare_result",
            "observability.show_version",
            "SBLR_OBSERVABILITY_SHOW_VERSION",
        ),
    ),
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--client", required=True)
    parser.add_argument("--operation", choices=("show-version", "show-wait-events", "show-object-detail", "source-map", "error-vector", "txn-begin", "txn-commit", "txn-rollback", "txn-rollback-to-savepoint", "txn-savepoint", "txn-release-savepoint", "psql-autonomous-frame", "transaction-reservation-release", "temporary-instance-cleanup", "cursor-open", "cursor-fetch", "cursor-close", "read-by-key", "read-range", "read-stream", "result-set-pass", "access-cursor-open", "access-cursor-fetch", "access-cursor-close", "insert", "update", "delete", "merge", "table-truncate", "table-analyze", "bulk-import-stream", "bulk-export-stream", "statement-batch", "atomic-cas", "atomic-rmw", "advisory-lock", "advisory-lock-release", "function-call", "operator-call", "cast", "compare", "domain-operation", "udr-invoke", "procedure-invoke", "function-invoke", "aggregate-invoke", "sequence-nextval", "sequence-currval", "sequence-setval", "query-numeric", "advanced-datatype-family", "project", "aggregate", "group", "sort", "limit", "window", "return-result-set", "kv-structured-read", "kv-structured-mutate", "kv-structured-scan", "kv-structured-stream-read", "kv-structured-stream-append", "kv-structured-timeseries", "system-config-set", "ddl-create-domain", "ddl-alter-domain", "ddl-create-view", "ddl-alter-view", "ddl-drop-view", "ddl-create-trigger", "ddl-create-schema", "ddl-create-table", "ddl-create-index", "ddl-drop-index"),
                        default="source-map")
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "system-config-get", "system-config-reset", "ddl-create-rule", "ddl-drop-rule", "ddl-create-publication", "ddl-alter-publication", "ddl-drop-publication", "ddl-create-subscription", "ddl-alter-subscription", "ddl-drop-subscription", "ddl-create-operator", "ddl-drop-operator", "ddl-create-operator-class", "ddl-drop-operator-class", "ddl-create-operator-family", "ddl-alter-operator-family", "ddl-drop-operator-family", "ddl-drop-cast", "ddl-create-extension", "ddl-alter-extension", "ddl-drop-extension", "cluster-create-placement-policy", "cluster-alter-placement-policy", "cluster-drop-placement-policy", "versioned-branch-create", "versioned-branch-delete", "versioned-diff", "versioned-tag", "versioned-revert", "versioned-reset", "bitemporal-as-of", "verifiable-history-prove", "verify-proof-descriptor", "versioned-merge", "versioned-hash-read", "versioned-status-read", "accel-llvm-policy-set", "accel-llvm-compile", "accel-gpu-compile", "accel-llvm-inspect", "accel-llvm-invalidate", "accel-gpu-policy-set", "accel-gpu-inspect", "accel-gpu-invalidate", "bridge-describe-capabilities", "bridge-open-channel", "bridge-authenticate", "bridge-open-session", "bridge-close-session", "bridge-health", "bridge-begin-transaction", "bridge-commit-transaction", "bridge-rollback-transaction"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "ddl-alter-trigger", "ddl-refresh-materialized-view", "ddl-create-materialized-view", "ddl-drop-materialized-view", "ddl-drop-package", "ddl-drop-synonym", "ddl-drop-foreign-table", "ddl-alter-package", "ddl-alter-sequence", "ddl-drop-sequence", "ddl-create-type", "ddl-alter-type", "ddl-drop-type", "ddl-drop-table", "ddl-create-table-as-query-with-data", "ddl-create-table-as-query-with-no-data", "ddl-create-sequence"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "dml-counter-add", "dml-conditional-mutate", "ddl-alter-timeseries-value-cache", "ddl-drop-timeseries-value-cache"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "dml-timeseries-schema-write"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "ddl-timeseries-series-cardinality-policy"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "ddl-create-timeseries-value-cache"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "ddl-drop-trigger"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "security-drop-policy", "security-alter-policy", "security-drop-user", "security-authenticate", "security-deauthenticate", "session-role-switch", "session-setting-set", "session-setting-reset", "session-setting-get", "session-default-qualifier-set", "session-discard", "session-snapshot-handle", "context-set", "context-unset", "context-get", "stmt-prepare", "stmt-execute", "stmt-execute-direct", "stmt-free", "stmt-cancel", "parameter-bind", "result-page", "query-execute", "query-explain", "name-resolve", "parse-text", "catalog-epoch-check", "database-attach", "database-detach", "database-checkpoint", "database-vacuum", "database-alter", "lifecycle-create-database", "lifecycle-open-database", "lifecycle-attach-database", "lifecycle-detach-database", "lifecycle-enter-maintenance", "lifecycle-exit-maintenance", "lifecycle-enter-restricted-open", "lifecycle-exit-restricted-open", "lifecycle-inspect-database", "lifecycle-verify-database", "lifecycle-repair-database", "lifecycle-shutdown-database", "lifecycle-shutdown-force", "lifecycle-shutdown-acknowledge", "lifecycle-drop-database", "repl-consumer-subscribe", "repl-consumer-resume", "repl-consumer-pause", "repl-consumer-cancel", "repl-cdc-receive", "repl-cdc-ack", "repl-2pc-prewrite", "repl-2pc-commit", "repl-2pc-cleanup", "repl-2pc-resolve-lock", "repl-2pc-pessimistic-lock", "repl-2pc-pessimistic-rollback", "repl-2pc-heartbeat", "repl-2pc-check-status", "graph-traverse", "graph-optional-match"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "security-alter-role"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "graph-create"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "graph-merge"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "graph-set"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "graph-remove"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "graph-delete"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "graph-detach-delete"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "fulltext-score"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "fulltext-phrase-score"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "fulltext-multi-field-score"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "fulltext-regex-match"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "fulltext-wildcard-match"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "fulltext-prefix-match"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "fulltext-analyzer-apply"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "diagnostic-refusal"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "diagnostic-reset"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "descriptor-transform"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "migration-begin-donor"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "migration-alter"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "show-migration"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "migration-cutover"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "migration-rollback"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "migration-retain-evidence"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "internal-trigger-dispatch"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "internal-exception-raise"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "internal-exception-resignal"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "security-create-group-mapping"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "security-drop-group-mapping"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "security-grant"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "security-revoke"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "ddl-create-procedure"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "ddl-drop-fdw"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "ddl-alter-procedure", "ddl-drop-procedure", "ddl-create-function", "ddl-alter-function", "ddl-drop-function", "ddl-create-package", "ddl-create-temporary-table", "ddl-drop-temporary-table", "ddl-rename-object-vector", "ddl-rename-object", "ddl-create-synonym", "ddl-create-foreign-table", "ddl-create-fdw", "ddl-create-or-replace-srs", "ddl-drop-srs", "ddl-create-rewrite-rule", "ddl-alter-rewrite-rule", "ddl-drop-rewrite-rule", "ddl-validate-constraint", "security-create-privilege-template", "security-create-user", "security-alter-user", "security-create-role", "security-create-policy", "security-drop-role", "security-alter-privilege-template", "security-drop-privilege-template", "database-create-template-clone", "ddl-create-aggregate", "ddl-alter-aggregate", "ddl-drop-aggregate", "ddl-purge-system-history", "ddl-set-index-optimizer-eligibility", "ddl-set-table-type-enforcement", "database-serialize-logical-snapshot"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "database-deserialize-logical-snapshot"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "ddl-create-macro", "ddl-create-dictionary", "ddl-drop-dictionary", "ddl-alter-dictionary", "ddl-create-continuous-view", "ddl-alter-continuous-view", "ddl-drop-continuous-view", "dml-async-insert-submit", "dml-async-insert-status", "dml-async-insert-cancel", "dml-counter-add", "ddl-drop-macro", "admin-register-external-relation-resolver", "admin-unregister-external-relation-resolver"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "alter-gpu-profile-disable"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "filespace-create"))
    parser.add_argument("--work-dir", required=True)
    parser.add_argument("--cluster-provider-proof")
    parser.add_argument("--expected-cluster-provider-mode")
    parser.add_argument("--expected-cluster-provider-diagnostic")
    args = parser.parse_args()
    work = allocate_work(Path(args.work_dir))
    server = None
    try:
        if args.cluster_provider_proof:
            if not (
                args.expected_cluster_provider_mode
                and args.expected_cluster_provider_diagnostic
            ):
                raise ProofError(
                    "cluster provider proof requires exact mode and diagnostic expectations"
                )
            proof = subprocess.run(
                [
                    args.cluster_provider_proof,
                    args.expected_cluster_provider_mode,
                    args.expected_cluster_provider_diagnostic,
                ],
                capture_output=True,
                text=True,
                timeout=30,
            )
            expected_support = (
                "compile_link_only"
                if args.expected_cluster_provider_mode == "compile_link_stub"
                else "not_enabled"
            )
            expected_markers = (
                "cluster_provider_proof=passed",
                f"cluster_provider_mode={args.expected_cluster_provider_mode}",
                f"cluster_provider_support={expected_support}",
                "cluster_provider_diagnostic="
                f"{args.expected_cluster_provider_diagnostic}",
            )
            if proof.returncode != 0 or not all(
                marker in proof.stdout for marker in expected_markers
            ):
                raise ProofError(
                    "configured cluster provider proof failed: "
                    f"{proof.stdout}{proof.stderr}"
                )
        database = work / "source_map.sbdb"
        endpoint = work / "sc" / "s.sock"
        evidence = seed_database(
            Path(args.server),
            database,
            ("--bulk-import-fixture",)
            if args.operation == "bulk-import-stream"
            else (),
        )
        server_trace = work / "server_phase.jsonl"
        dispatch_trace = work / "dispatch_phase.jsonl"
        env = os.environ.copy()
        env["SCRATCHBIRD_SERVER_EXECUTE_PHASE_TRACE_FILE"] = str(server_trace)
        env["SCRATCHBIRD_ENGINE_ABI_PHASE_TRACE_FILE"] = str(dispatch_trace)
        env["SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE"] = str(dispatch_trace)
        server = subprocess.Popen(
            [args.server, "--foreground", "--no-listeners", "--control-dir",
             str(work / "sc"), "--runtime-dir", str(work / "sr"),
             "--database", str(database), "--sbps-endpoint", str(endpoint)],
            stdout=(work / "server.out").open("wb"),
            stderr=(work / "server.err").open("wb"), env=env,
        )
        wait_unix(endpoint)
        catalog_event_path = Path(f"{database}.sb.catalog_object_events")
        catalog_event_before = None
        executor_availability_before = None
        executor_availability_pattern = (
            f"{database.name}.sb.sblr_executor_availability_registry.v1*"
        )

        def executor_availability_snapshot() -> dict[str, bytes]:
            return {
                path.name: path.read_bytes()
                for path in sorted(database.parent.glob(
                    executor_availability_pattern
                ))
                if path.is_file()
            }

        if args.operation in STATIC_EXECUTOR_EVIDENCE_REFUSALS:
            if not catalog_event_path.exists():
                raise ProofError(
                    "canonical catalog-object event journal is unavailable"
                )
            bootstrap = subprocess.run(
                [
                    args.client,
                    f"unix:{endpoint}",
                    str(database),
                    "alice",
                    evidence,
                    "show-version",
                    "sbsql-sblr-static-refusal-bootstrap",
                ],
                capture_output=True,
                text=True,
                timeout=30,
            )
            if bootstrap.returncode != 0 or bootstrap.stderr:
                raise ProofError(
                    "neutral authenticated executor-availability bootstrap "
                    f"failed: {bootstrap.stdout}{bootstrap.stderr}"
                )
            # A second neutral authenticated session settles any normal
            # session-owned availability rows materialized asynchronously by
            # the first attach.  Snapshot only after that baseline is stable;
            # the target refusal itself must still leave every byte unchanged.
            bootstrap = subprocess.run(
                [
                    args.client,
                    f"unix:{endpoint}",
                    str(database),
                    "alice",
                    evidence,
                    "show-version",
                    "sbsql-sblr-static-refusal-bootstrap-stable",
                ],
                capture_output=True,
                text=True,
                timeout=30,
            )
            if bootstrap.returncode != 0 or bootstrap.stderr:
                raise ProofError(
                    "stable neutral authenticated executor-availability "
                    f"bootstrap failed: {bootstrap.stdout}{bootstrap.stderr}"
                )
            for trace_path in (server_trace, dispatch_trace):
                if trace_path.exists():
                    trace_path.write_bytes(b"")
            catalog_event_before = catalog_event_path.read_bytes()
            executor_availability_before = executor_availability_snapshot()
        command = [args.client, f"unix:{endpoint}", str(database), "alice",
                   evidence, args.operation, f"sbsql-sblr-{args.operation}-e2e-first"]
        first = subprocess.run(command, capture_output=True, text=True, timeout=30)
        if first.returncode != 0:
            raise ProofError(f"explicit source-map operation failed: {first.stdout}{first.stderr}")
        if args.operation == "ddl-create-index":
            expected_refusal = (
                "CSC-TEST-002601 DDL_CREATE_INDEX "
                "deterministic_refusal=SBSQL.IMPL.NOT_AVAILABLE "
                "no_canonical_execution=true\n"
            )
            if first.stdout != expected_refusal or first.stderr:
                raise ProofError(
                    "CREATE INDEX did not return the exact deterministic "
                    "SBSQL.IMPL.NOT_AVAILABLE refusal"
                )
        elif args.operation in (
            "ddl-create-type",
            "ddl-alter-type",
            "ddl-drop-type",
        ):
            test_id, operation_label = {
                "ddl-create-type": ("CSC-TEST-002673", "DDL_CREATE_TYPE"),
                "ddl-alter-type": ("CSC-TEST-002675", "DDL_ALTER_TYPE"),
                "ddl-drop-type": ("CSC-TEST-002677", "DDL_DROP_TYPE"),
            }[args.operation]
            expected_refusal = (
                f"{test_id} {operation_label} "
                "deterministic_refusal=SBSQL.IMPL.NOT_AVAILABLE "
                "no_canonical_execution=true\n"
            )
            if first.stdout != expected_refusal or first.stderr:
                raise ProofError(
                    f"{operation_label} did not return the exact deterministic "
                    "SBSQL.IMPL.NOT_AVAILABLE refusal"
                )
        elif args.operation in PRE_CONTEXT_COMMAND_REFUSALS:
            test_id, operation_label, _ = (
                PRE_CONTEXT_COMMAND_REFUSALS[args.operation]
            )
            expected_refusal = (
                f"{test_id} {operation_label} "
                "deterministic_refusal=SBSQL.IMPL.NOT_AVAILABLE "
                "no_statement_context=true no_canonical_execution=true "
                "no_result_publication=true\n"
            )
            if first.stdout != expected_refusal or first.stderr:
                raise ProofError(
                    f"{operation_label} did not return the exact pre-context "
                    "SBSQL.IMPL.NOT_AVAILABLE refusal"
                )
        elif args.operation in (
            "ddl-alter-subscription",
            "ddl-drop-subscription",
        ):
            expected_refusal = (
                "CSC-TEST-003993 DDL_ALTER_SUBSCRIPTION "
                "deterministic_cluster_refusal\n"
                if args.operation == "ddl-alter-subscription"
                else "CSC-TEST-003997 DDL_DROP_SUBSCRIPTION "
                "deterministic_cluster_refusal\n"
            )
            if first.stdout != expected_refusal or first.stderr:
                raise ProofError(
                    "subscription cluster refusal did not return its exact "
                    "single-diagnostic/no-result proof"
                )
        elif args.operation in STATIC_EXECUTOR_EVIDENCE_REFUSALS:
            test_id, operation_label, _, _, _ = (
                STATIC_EXECUTOR_EVIDENCE_REFUSALS[args.operation]
            )
            expected_refusal = (
                f"{test_id} {operation_label} "
                "deterministic_refusal="
                "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING "
                "no_engine_dispatch=true no_result_publication=true\n"
            )
            if first.stdout != expected_refusal or first.stderr:
                raise ProofError(
                    f"{operation_label} did not return the exact static "
                    "executor-evidence refusal"
                )
        audit_paths = (server_trace, dispatch_trace,
                       work / "sc" / "sb_server.audit.jsonl")
        audit = "\n".join(p.read_text(encoding="utf-8", errors="replace")
                          for p in audit_paths if p.exists())
        if args.operation == "show-version":
            expected = ("operation_id=observability.show_version",
                        "opcode=SBLR_OBSERVABILITY_SHOW_VERSION",
                        "opcode_code=3334")
        elif args.operation == "show-wait-events":
            expected = ()
        elif args.operation == "show-object-detail":
            expected = ("operation_id=engine.op.catalog_introspect", "opcode=SBLR_CATALOG_INTROSPECT", "opcode_code=4864")
        elif args.operation == "source-map":
            expected = ("executor_id=engine.op.source_map", "opcode=SBLR_SOURCE_MAP",
                        "opcode_code=6")
        elif args.operation == "error-vector":
            expected = ("executor_id=engine.op.error_vector", "opcode=SBLR_ERROR_VECTOR",
                        "opcode_code=7")
        elif args.operation == "txn-commit":
            expected = ("executor_id=engine.op.txn_commit", "opcode=SBLR_TXN_COMMIT",
                        "opcode_code=257", "operand_descriptor_id=transaction_handle_and_commit_options",
                        "result_descriptor_id=commit_result", "result_descriptor_version=1",
                        "commit_result_sha256=", "executor_availability_generation=")
        elif args.operation == "txn-rollback":
            expected = ("executor_id=engine.op.txn_rollback", "opcode=SBLR_TXN_ROLLBACK",
                        "opcode_code=258", "operand_descriptor_id=transaction_handle_and_rollback_options",
                        "result_descriptor_id=rollback_result", "result_descriptor_version=1",
                        "rollback_result_sha256=", "executor_availability_generation=")
        elif args.operation == "txn-savepoint":
            expected = ("executor_id=engine.op.txn_savepoint", "opcode=SBLR_TXN_SAVEPOINT",
                        "opcode_code=259", "operand_descriptor_id=savepoint_descriptor",
                        "result_descriptor_id=savepoint_handle", "result_descriptor_version=1",
                        "savepoint_handle_sha256=", "executor_availability_generation=")
        elif args.operation == "txn-release-savepoint":
            expected = ("executor_id=engine.op.txn_release_savepoint",
                        "opcode=SBLR_TXN_RELEASE_SAVEPOINT", "opcode_code=260",
                        "operand_descriptor_id=savepoint_release_handle",
                        "result_descriptor_id=savepoint_release_result",
                        "result_descriptor_version=1", "release_result_sha256=",
                        "executor_availability_generation=")
        elif args.operation == "txn-rollback-to-savepoint":
            expected = ("executor_id=engine.op.txn_rollback_to_savepoint", "opcode=SBLR_TXN_ROLLBACK_TO_SAVEPOINT",
                        "opcode_code=261", "operand_descriptor_id=savepoint_rollback_handle",
                        "result_descriptor_id=savepoint_rollback_result", "result_descriptor_version=1",
                        "rollback_to_savepoint_result_sha256=", "executor_availability_generation=")
        elif args.operation == "psql-autonomous-frame":
            expected = ("executor_id=engine.op.psql_autonomous_frame", "opcode=SBLR_PSQL_AUTONOMOUS_FRAME",
                        "opcode_code=262", "operand_descriptor_id=autonomous_frame_descriptor",
                        "result_descriptor_id=autonomous_frame_result", "result_descriptor_version=1",
                        "autonomous_frame_result_sha256=", "executor_availability_generation=")
        elif args.operation == "transaction-reservation-release":
            expected = ("executor_id=engine.op.transaction_reservation_release",
                        "opcode=SBLR_TRANSACTION_RESERVATION_RELEASE", "opcode_code=263",
                        "operand_descriptor_id=relation_reservation_release_descriptor",
                        "result_descriptor_id=transaction_reservation_result",
                        "result_descriptor_version=1", "reservation_release_result_sha256=",
                        "executor_availability_generation=")
        elif args.operation == "temporary-instance-cleanup":
            expected = ("executor_id=engine.op.temporary_instance_cleanup", "opcode=SBLR_TEMPORARY_INSTANCE_CLEANUP", "opcode_code=264", "operand_descriptor_id=temporary_instance_cleanup_descriptor", "result_descriptor_id=temporary_cleanup_result", "result_descriptor_version=1", "temporary_cleanup_result_sha256=", "executor_availability_generation=")
        elif args.operation == "cursor-open":
            expected = ("executor_id=engine.op.cursor_open", "opcode=SBLR_CURSOR_OPEN",
                        "opcode_code=512", "operand_descriptor_id=cursor_open_plan_ref",
                        "result_descriptor_id=cursor_handle", "result_descriptor_version=1",
                        "cursor_handle_sha256=", "executor_availability_generation=")
        elif args.operation == "cursor-fetch":
            expected = ("executor_id=engine.op.cursor_fetch", "opcode=SBLR_CURSOR_FETCH", "opcode_code=513", "operand_descriptor_id=cursor_fetch_handle", "result_descriptor_id=cursor_fetch_result", "result_descriptor_version=1", "cursor_fetch_result_sha256=", "executor_availability_generation=")
        elif args.operation == "cursor-close":
            expected = ("executor_id=engine.op.cursor_close", "opcode=SBLR_CURSOR_CLOSE", "opcode_code=514", "operand_descriptor_id=cursor_close_handle", "result_descriptor_id=cursor_close_result", "result_descriptor_version=1", "cursor_close_result_sha256=", "executor_availability_generation=")
        elif args.operation == "read-by-key":
            expected = ("executor_id=engine.op.read_by_key", "opcode=SBLR_READ_BY_KEY", "opcode_code=515", "operand_descriptor_id=uuid_object_key_descriptor", "result_descriptor_id=row_descriptor", "result_descriptor_version=1", "read_by_key_result_sha256=", "executor_availability_generation=")
        elif args.operation == "read-range":
            expected = ("executor_id=engine.op.read_range", "opcode=SBLR_READ_RANGE", "opcode_code=516", "operand_descriptor_id=range_scan_descriptor", "result_descriptor_id=rowset_descriptor", "result_descriptor_version=1", "read_range_result_sha256=", "executor_availability_generation=")
        elif args.operation == "read-stream":
            expected = ("executor_id=engine.op.read_stream", "opcode=SBLR_READ_STREAM", "opcode_code=517", "operand_descriptor_id=stream_descriptor", "result_descriptor_id=stream_handle", "result_descriptor_version=1", "read_stream_handle_sha256=", "executor_availability_generation=")
        elif args.operation == "result-set-pass":
            expected = ("executor_id=engine.op.result_set_pass", "opcode=SBLR_RESULT_SET_PASS", "opcode_code=518", "operand_descriptor_id=result_set_handle_and_lifetime", "result_descriptor_id=result_set_handle", "result_descriptor_version=1", "result_set_pass_handle_sha256=", "executor_availability_generation=")
        elif args.operation == "access-cursor-open":
            expected = ("executor_id=engine.op.access_cursor_open", "opcode=SBLR_ACCESS_CURSOR_OPEN", "opcode_code=519", "operand_descriptor_id=access_cursor_open_descriptor", "result_descriptor_id=access_cursor_handle", "result_descriptor_version=1", "access_cursor_handle_sha256=", "executor_availability_generation=")
        elif args.operation == "access-cursor-fetch":
            expected = ("executor_id=engine.op.access_cursor_fetch", "opcode=SBLR_ACCESS_CURSOR_FETCH", "opcode_code=520", "operand_descriptor_id=access_cursor_fetch_descriptor", "result_descriptor_id=access_cursor_rowset_or_eof", "result_descriptor_version=1", "access_cursor_fetch_result_sha256=", "executor_availability_generation=")
        elif args.operation == "access-cursor-close":
            expected = ("executor_id=engine.op.access_cursor_close", "opcode=SBLR_ACCESS_CURSOR_CLOSE", "opcode_code=521", "operand_descriptor_id=access_cursor_close_descriptor", "result_descriptor_id=void", "result_descriptor_version=1", "access_cursor_close_evidence_sha256=", "executor_availability_generation=")
        elif args.operation == "insert":
            expected = ("executor_id=engine.op.insert", "opcode=SBLR_INSERT", "opcode_code=768", "operand_descriptor_id=insert_descriptor", "result_descriptor_id=mutation_result", "result_descriptor_version=1", "insert_result_sha256=", "executor_availability_generation=")
        elif args.operation == "update":
            expected = ("executor_id=engine.op.update", "opcode=SBLR_UPDATE", "opcode_code=769", "operand_descriptor_id=update_descriptor", "result_descriptor_id=mutation_result", "result_descriptor_version=1", "update_result_sha256=", "executor_availability_generation=")
        elif args.operation == "delete":
            expected = ("executor_id=engine.op.delete", "opcode=SBLR_DELETE", "opcode_code=770", "operand_descriptor_id=delete_descriptor", "result_descriptor_id=mutation_result", "result_descriptor_version=1", "delete_result_sha256=", "executor_availability_generation=")
        elif args.operation == "merge":
            expected = ("executor_id=engine.op.merge", "opcode=SBLR_MERGE", "opcode_code=771", "operand_descriptor_id=merge_descriptor", "result_descriptor_id=mutation_result", "result_descriptor_version=1", "merge_result_sha256=", "executor_availability_generation=")
        elif args.operation == "table-truncate":
            expected = ("executor_id=engine.op.table_truncate", "opcode=SBLR_TABLE_TRUNCATE", "opcode_code=773", "operand_descriptor_id=truncate_table_descriptor", "result_descriptor_id=mutation_result", "result_descriptor_version=1", "table_truncate_result_sha256=", "executor_availability_generation=")
        elif args.operation == "table-analyze":
            expected = ("executor_id=engine.op.table_analyze", "opcode=SBLR_TABLE_ANALYZE", "opcode_code=774", "operand_descriptor_id=analyze_table_descriptor", "result_descriptor_id=mutation_result", "result_descriptor_version=1", "table_analyze_result_sha256=", "executor_availability_generation=")
        elif args.operation == "bulk-import-stream":
            expected = ("executor_id=engine.op.bulk_import_stream", "opcode=SBLR_BULK_IMPORT_STREAM", "opcode_code=775", "operand_descriptor_id=bulk_import_stream_descriptor", "result_descriptor_id=bulk_mutation_result", "result_descriptor_version=1", "bulk_import_stream_result_sha256=", "executor_availability_generation=")
        elif args.operation == "bulk-export-stream":
            expected = ("executor_id=engine.op.bulk_export_stream", "opcode=SBLR_BULK_EXPORT_STREAM", "opcode_code=776", "operand_descriptor_id=bulk_export_stream_descriptor", "result_descriptor_id=bulk_read_result", "result_descriptor_version=1", "bulk_export_stream_result_sha256=", "executor_availability_generation=")
        elif args.operation == "statement-batch":
            expected = ("executor_id=engine.op.statement_batch", "opcode=SBLR_STATEMENT_BATCH", "opcode_code=777", "operand_descriptor_id=statement_batch_descriptor", "result_descriptor_id=batch_result_vector", "result_descriptor_version=1", "statement_batch_result_sha256=", "executor_availability_generation=")
        elif args.operation == "atomic-cas":
            expected = ("executor_id=engine.op.atomic_cas", "opcode=SBLR_ATOMIC_CAS", "opcode_code=778", "operand_descriptor_id=atomic_cas_descriptor", "result_descriptor_id=atomic_cas_result", "result_descriptor_version=1", "atomic_cas_result_sha256=", "executor_availability_generation=")
        elif args.operation == "atomic-rmw":
            expected = ("executor_id=engine.op.atomic_read_modify_write", "opcode=SBLR_ATOMIC_READ_MODIFY_WRITE", "opcode_code=779", "operand_descriptor_id=atomic_rmw_descriptor", "result_descriptor_id=atomic_rmw_result", "result_descriptor_version=1", "atomic_rmw_result_sha256=", "executor_availability_generation=")
        elif args.operation == "advisory-lock":
            expected = ("executor_id=engine.op.advisory_lock_acquire", "opcode=SBLR_ADVISORY_LOCK_ACQUIRE", "opcode_code=780", "operand_descriptor_id=advisory_lock_descriptor", "result_descriptor_id=advisory_lock_result", "result_descriptor_version=1", "advisory_lock_result_sha256=", "executor_availability_generation=")
        elif args.operation == "advisory-lock-release":
            expected = ("executor_id=engine.op.advisory_lock_release", "opcode=SBLR_ADVISORY_LOCK_RELEASE", "opcode_code=781", "operand_descriptor_id=advisory_lock_release_descriptor", "result_descriptor_id=advisory_lock_result", "result_descriptor_version=1", "advisory_lock_result_sha256=", "executor_availability_generation=")
        elif args.operation == "limit":
            expected = ("executor_id=engine.op.limit", "opcode=SBLR_LIMIT", "opcode_code=1284", "operand_descriptor_id=limit_descriptor", "result_descriptor_id=rowset_descriptor", "result_descriptor_version=1", "limit_result_sha256=", "executor_availability_generation=")
        elif args.operation == "window":
            expected = ("executor_id=engine.op.window", "opcode=SBLR_WINDOW", "opcode_code=1285", "operand_descriptor_id=window_descriptor", "result_descriptor_id=rowset_descriptor", "result_descriptor_version=1", "executor_availability_generation=")
        elif args.operation == "return-result-set":
            expected = ("executor_id=engine.op.return_result_set", "opcode=SBLR_RETURN_RESULT_SET", "opcode_code=1286", "operand_descriptor_id=result_set_return_descriptor", "result_descriptor_id=result_set_handle", "result_descriptor_version=1", "return_result_set_result_sha256=", "executor_availability_generation=")
        elif args.operation == "kv-structured-read":
            expected = ("executor_id=engine.op.kv_structured_read", "opcode=SBLR_KV_STRUCTURED_READ", "opcode_code=8192", "operand_descriptor_id=kv_structured_read_descriptor", "result_descriptor_id=kv_structured_result", "result_descriptor_version=1", "kv_structured_read_result_sha256=", "executor_availability_generation=")
        elif args.operation == "kv-structured-mutate":
            expected = ("executor_id=engine.op.kv_structured_mutate", "opcode=SBLR_KV_STRUCTURED_MUTATE", "opcode_code=8193", "operand_descriptor_id=kv_structured_mutate_descriptor", "result_descriptor_id=kv_structured_result", "result_descriptor_version=1", "kv_structured_mutate_result_sha256=", "executor_availability_generation=")
        elif args.operation == "kv-structured-scan":
            expected = ("executor_id=engine.op.kv_structured_scan", "opcode=SBLR_KV_STRUCTURED_SCAN", "opcode_code=8194", "operand_descriptor_id=kv_structured_scan_descriptor", "result_descriptor_id=kv_structured_result", "result_descriptor_version=1", "kv_structured_scan_result_sha256=", "executor_availability_generation=")
        elif args.operation == "kv-structured-stream-read":
            expected = ("executor_id=engine.op.kv_structured_stream_read", "opcode=SBLR_KV_STRUCTURED_STREAM_READ", "opcode_code=8195", "operand_descriptor_id=kv_structured_stream_read_descriptor", "result_descriptor_id=kv_structured_result", "result_descriptor_version=1", "kv_structured_stream_read_result_sha256=", "executor_availability_generation=")
        elif args.operation == "kv-structured-stream-append":
            expected = ("executor_id=engine.op.kv_structured_stream_append", "opcode=SBLR_KV_STRUCTURED_STREAM_APPEND", "opcode_code=8196", "operand_descriptor_id=kv_structured_stream_append_descriptor", "result_descriptor_id=kv_structured_mutation_result", "result_descriptor_version=1", "kv_structured_stream_append_result_sha256=", "executor_availability_generation=")
        elif args.operation == "kv-structured-timeseries":
            expected = ()
        elif args.operation == "system-config-get":
            expected = ()
        elif args.operation == "system-config-reset":
            expected = ()
        elif args.operation == "ddl-create-rule":
            expected = ()
        elif args.operation == "ddl-drop-rule":
            expected = ()
        elif args.operation == "ddl-create-publication":
            expected = ()
        elif args.operation == "ddl-alter-publication":
            expected = ()
        elif args.operation == "ddl-drop-publication":
            expected = ()
        elif args.operation == "ddl-create-subscription":
            expected = ()
        elif args.operation == "ddl-alter-subscription":
            expected = ()
        elif args.operation == "ddl-drop-subscription":
            expected = ()
        elif args.operation == "ddl-create-operator":
            expected = ()
        elif args.operation == "ddl-drop-operator":
            expected = ()
        elif args.operation == "ddl-create-operator-class":
            expected = ()
        elif args.operation == "ddl-drop-operator-class":
            expected = ()
        elif args.operation == "ddl-create-operator-family":
            expected = ()
        elif args.operation == "ddl-alter-operator-family":
            expected = ()
        elif args.operation == "ddl-drop-operator-family":
            expected = ()
        elif args.operation == "ddl-drop-cast":
            expected = ()
        elif args.operation == "ddl-create-extension":
            expected = ()
        elif args.operation == "ddl-alter-extension":
            expected = ()
        elif args.operation == "ddl-drop-extension":
            expected = ()
        elif args.operation == "cluster-create-placement-policy":
            expected = ()
        elif args.operation in ("cluster-alter-placement-policy", "cluster-drop-placement-policy"):
            expected = ()
        elif args.operation == "versioned-branch-create":
            expected = ()
        elif args.operation == "versioned-branch-delete":
            expected = ()
        elif args.operation == "versioned-diff":
            expected = ()
        elif args.operation == "versioned-tag":
            expected = ()
        elif args.operation == "versioned-revert":
            expected = ()
        elif args.operation == "versioned-reset":
            expected = ()
        elif args.operation == "accel-llvm-policy-set":
            expected = ()
        elif args.operation == "accel-llvm-compile":
            expected = ()
        elif args.operation == "accel-gpu-compile":
            expected = ()
        elif args.operation == "accel-llvm-inspect":
            expected = ()
        elif args.operation in ("accel-llvm-invalidate", "accel-gpu-policy-set", "accel-gpu-inspect", "accel-gpu-invalidate", "bridge-describe-capabilities", "bridge-open-channel", "bridge-authenticate", "bridge-open-session", "bridge-close-session", "bridge-health", "bridge-begin-transaction", "bridge-commit-transaction", "bridge-rollback-transaction"):
            expected = ()
        elif args.operation == "system-config-set":
            expected = ("executor_id=engine.op.system_config_set", "opcode=SBLR_SYSTEM_CONFIG_SET", "opcode_code=5125", "operand_descriptor_id=system_config_set_descriptor", "result_descriptor_id=management_result", "result_descriptor_version=1", "system_config_set_result_sha256=", "executor_availability_generation=")
        elif args.operation == "ddl-create-domain":
            expected = ("executor_id=engine.op.ddl_create_domain", "opcode=SBLR_DDL_CREATE_DOMAIN", "opcode_code=1542", "operand_descriptor_id=create_domain_descriptor", "result_descriptor_id=ddl_result", "result_descriptor_version=1", "ddl_create_domain_result_sha256=", "executor_availability_generation=")
        elif args.operation == "ddl-alter-domain":
            expected = ("executor_id=engine.op.ddl_alter_domain", "opcode=SBLR_DDL_ALTER_DOMAIN", "opcode_code=1547", "operand_descriptor_id=alter_domain_descriptor", "result_descriptor_id=ddl_result", "result_descriptor_version=1", "ddl_alter_domain_result_sha256=", "executor_availability_generation=")
        elif args.operation == "ddl-create-sequence":
            expected = ("executor_id=engine.op.ddl_create_sequence", "opcode=SBLR_DDL_CREATE_SEQUENCE", "opcode_code=1671", "operand_descriptor_id=create_sequence_descriptor", "result_descriptor_id=ddl_result", "result_descriptor_version=1", "ddl_create_sequence_result_sha256=", "executor_availability_generation=")
        elif args.operation == "ddl-create-view":
            expected = ()
        elif args.operation == "ddl-alter-view":
            expected = ()
        elif args.operation == "ddl-drop-view":
            expected = ()
        elif args.operation == "ddl-refresh-materialized-view":
            expected = ()
        elif args.operation == "ddl-create-materialized-view":
            expected = ("executor_id=engine.op.ddl_create_materialized_view", "opcode=SBLR_DDL_CREATE_MATERIALIZED_VIEW", "opcode_code=1566", "operand_descriptor_id=create_materialized_view_descriptor", "result_descriptor_id=ddl_result", "result_descriptor_version=1", "executor_availability_generation=")
        elif args.operation == "ddl-drop-materialized-view":
            expected = ()
        elif args.operation == "ddl-drop-package":
            expected = ()
        elif args.operation == "ddl-alter-package":
            expected = ()
        elif args.operation == "ddl-alter-sequence":
            expected = ("executor_id=engine.op.ddl_alter_sequence", "opcode=SBLR_DDL_ALTER_SEQUENCE", "opcode_code=1564", "operand_descriptor_id=alter_sequence_descriptor", "result_descriptor_id=ddl_result", "result_descriptor_version=1", "executor_availability_generation=")
        elif args.operation == "ddl-drop-sequence":
            expected = ("executor_id=engine.op.ddl_drop_sequence", "opcode=SBLR_DDL_DROP_SEQUENCE", "opcode_code=1565", "operand_descriptor_id=drop_sequence_descriptor", "result_descriptor_id=ddl_result", "result_descriptor_version=1", "executor_availability_generation=")
        elif args.operation == "ddl-create-type":
            expected = ()
        elif args.operation in PRE_CONTEXT_COMMAND_REFUSALS:
            expected = ()
        elif args.operation in STATIC_EXECUTOR_EVIDENCE_REFUSALS:
            expected = ()
        elif args.operation == "security-create-privilege-template":
            expected = ()
        elif args.operation == "security-create-user":
            expected = ()
        elif args.operation == "security-alter-user":
            expected = ()
        elif args.operation == "security-alter-privilege-template":
            expected = ()
        elif args.operation == "security-drop-privilege-template":
            expected = ()
        elif args.operation == "database-create-template-clone":
            expected = ()
        elif args.operation in ("ddl-create-aggregate", "ddl-alter-aggregate", "ddl-drop-aggregate", "ddl-purge-system-history", "ddl-set-index-optimizer-eligibility", "ddl-set-table-type-enforcement", "database-serialize-logical-snapshot"):
            expected = ()
        elif args.operation == "database-deserialize-logical-snapshot":
            expected = ()
        elif args.operation == "ddl-create-macro":
            expected = ()
        elif args.operation == "ddl-create-dictionary":
            expected = ()
        elif args.operation == "ddl-drop-dictionary":
            expected = ()
        elif args.operation == "ddl-alter-dictionary":
            expected = ()
        elif args.operation == "ddl-create-continuous-view":
            expected = ()
        elif args.operation == "ddl-alter-continuous-view":
            expected = ()
        elif args.operation == "ddl-drop-continuous-view":
            expected = ()
        elif args.operation == "dml-async-insert-submit":
            expected = ()
        elif args.operation in ("dml-async-insert-status", "dml-async-insert-cancel"):
            expected = ()
        elif args.operation == "dml-counter-add":
            expected = ()
        elif args.operation == "dml-conditional-mutate":
            expected = ()
        elif args.operation == "dml-timeseries-schema-write":
            expected = ()
        elif args.operation == "ddl-alter-timeseries-value-cache":
            expected = ()
        elif args.operation == "ddl-drop-timeseries-value-cache":
            expected = ()
        elif args.operation == "alter-gpu-profile-disable":
            expected = ()
        elif args.operation == "filespace-create":
            expected = ()
        elif args.operation == "diagnostic-refusal":
            expected = ()
        elif args.operation == "diagnostic-reset":
            expected = ()
        elif args.operation == "migration-begin-donor":
            expected = ()
        elif args.operation == "migration-alter":
            expected = ()
        elif args.operation == "show-migration":
            expected = ()
        elif args.operation == "migration-cutover":
            expected = ()
        elif args.operation == "migration-rollback":
            expected = ()
        elif args.operation == "migration-retain-evidence":
            expected = ()
        elif args.operation == "internal-trigger-dispatch":
            expected = ()
        elif args.operation == "internal-exception-raise":
            expected = ()
        elif args.operation == "internal-exception-resignal":
            expected = ()
        elif args.operation == "ddl-timeseries-series-cardinality-policy":
            expected = ()
        elif args.operation == "ddl-create-timeseries-value-cache":
            expected = ()
        elif args.operation == "ddl-drop-macro":
            expected = ()
        elif args.operation in ("ddl-create-type", "ddl-alter-type", "ddl-drop-type"):
            expected = ()
        elif args.operation == "admin-register-external-relation-resolver":
            expected = ()
        elif args.operation == "admin-unregister-external-relation-resolver":
            expected = ()
        elif args.operation == "ddl-drop-fdw":
            expected = ()
        elif args.operation in ("ddl-create-trigger", "ddl-alter-trigger", "ddl-drop-trigger", "ddl-create-procedure", "ddl-alter-procedure", "ddl-drop-procedure", "ddl-create-function", "ddl-alter-function", "ddl-drop-function", "ddl-create-package", "ddl-create-temporary-table", "ddl-create-foreign-table", "ddl-create-fdw", "ddl-drop-temporary-table", "ddl-rename-object-vector", "ddl-rename-object", "ddl-create-synonym", "ddl-create-or-replace-srs", "ddl-drop-srs", "ddl-create-rewrite-rule"):
            expected = ()
        elif args.operation == "ddl-create-schema":
            expected = ("executor_id=engine.op.ddl_create_schema", "opcode=SBLR_DDL_CREATE_SCHEMA", "opcode_code=1536", "operand_descriptor_id=create_schema_descriptor", "result_descriptor_id=ddl_result", "result_descriptor_version=1", "ddl_create_schema_result_sha256=", "executor_availability_generation=")
        elif args.operation == "ddl-create-table":
            expected = ("executor_id=engine.op.ddl_create_table", "opcode=SBLR_DDL_CREATE_TABLE", "opcode_code=1537", "operand_descriptor_id=create_table_descriptor", "result_descriptor_id=ddl_result", "result_descriptor_version=1", "ddl_create_table_result_sha256=", "executor_availability_generation=")
        elif args.operation == "ddl-create-index":
            expected = ()
        elif args.operation == "ddl-drop-index":
            expected = ("executor_id=engine.op.ddl_drop_index", "opcode=SBLR_DDL_DROP_INDEX", "opcode_code=1541", "operand_descriptor_id=drop_index_descriptor", "result_descriptor_id=ddl_result", "result_descriptor_version=1", "ddl_drop_index_result_sha256=", "executor_availability_generation=")
        else:
            expected = ("executor_id=engine.op.txn_begin", "opcode=SBLR_TXN_BEGIN",
                        "opcode_code=256", "operand_descriptor_id=transaction_begin_options",
                        "result_descriptor_id=transaction_handle",
                        "result_descriptor_version=1", "transaction_handle_sha256=",
                        "executor_availability_generation=")
        required_markers = (*expected, "parent_success_barrier=passed")
        if args.operation == "ddl-create-index" or args.operation in (
            "ddl-create-type",
            "ddl-alter-type",
            "ddl-drop-type",
        ):
            required_markers = ()
        if args.operation in STATIC_EXECUTOR_EVIDENCE_REFUSALS:
            required_markers = ()
        if args.operation in PRE_CONTEXT_COMMAND_REFUSALS:
            required_markers = ()
        if args.operation in ("filespace-create", "ddl-create-subscription", "ddl-alter-subscription", "ddl-drop-subscription", "ddl-create-operator-class", "ddl-drop-operator-class", "ddl-create-operator-family", "ddl-alter-operator-family", "ddl-drop-operator-family", "ddl-drop-cast", "ddl-create-extension", "ddl-alter-extension", "ddl-drop-extension", "cluster-create-placement-policy", "cluster-alter-placement-policy", "cluster-drop-placement-policy", "versioned-branch-create", "versioned-branch-delete", "versioned-diff", "versioned-tag", "versioned-revert", "versioned-reset", "bitemporal-as-of", "verifiable-history-prove", "verify-proof-descriptor", "versioned-merge", "versioned-hash-read", "versioned-status-read", "accel-llvm-policy-set", "accel-llvm-compile", "accel-gpu-compile", "accel-llvm-inspect", "accel-llvm-invalidate", "accel-gpu-policy-set", "accel-gpu-inspect", "accel-gpu-invalidate", "bridge-describe-capabilities", "bridge-open-channel", "bridge-authenticate", "bridge-open-session", "bridge-close-session", "bridge-health", "bridge-begin-transaction", "bridge-commit-transaction", "bridge-rollback-transaction"):
            # This is a standalone cluster-gated refusal proof.  No local
            # executor receipt exists, so the transaction success barrier is
            # intentionally not required for this refusal-only route.
            required_markers = ()
        for marker in required_markers:
            if marker not in audit:
                raise ProofError(f"source-map success evidence lacks {marker}")
        second = command.copy()
        second[-1] = f"sbsql-sblr-{args.operation}-e2e-independent"
        verified = subprocess.run(second, capture_output=True, text=True, timeout=30)
        if verified.returncode != 0 or (verified.stdout != first.stdout and args.operation != "ddl-drop-operator"):
            raise ProofError("independent authenticated process/receipt verification failed")
        if args.operation in PRE_CONTEXT_COMMAND_REFUSALS:
            if verified.stderr:
                raise ProofError(
                    "independent pre-context command refusal emitted "
                    "unexpected stderr"
                )
            nonempty_phase_traces = tuple(
                str(path)
                for path in (server_trace, dispatch_trace)
                if path.exists() and path.stat().st_size != 0
            )
            if nonempty_phase_traces:
                raise ProofError(
                    "pre-context command refusal reached statement-context, "
                    "executor, result, or publication tracing: "
                    f"{', '.join(nonempty_phase_traces)}"
                )
            _, operation_label, operation_markers = (
                PRE_CONTEXT_COMMAND_REFUSALS[args.operation]
            )
            final_audit = "\n".join(
                path.read_text(encoding="utf-8", errors="replace")
                for path in audit_paths
                if path.exists()
            )
            forbidden_markers = (
                *operation_markers,
                "parent_success_barrier=passed",
                "result_descriptor_id=",
                "result_descriptor_version=",
                "executor_availability_generation=",
                "_result_sha256=",
            )
            leaked_markers = tuple(
                marker for marker in forbidden_markers
                if marker in final_audit
            )
            if leaked_markers:
                raise ProofError(
                    f"{operation_label} refusal leaked statement-context, "
                    "canonical executor, result, or publication evidence: "
                    f"{', '.join(leaked_markers)}"
                )
        if args.operation in STATIC_EXECUTOR_EVIDENCE_REFUSALS:
            if verified.stderr:
                raise ProofError(
                    "independent static executor-evidence refusal emitted "
                    "unexpected stderr"
                )
            _, operation_label, operation_id, opcode, opcode_code = (
                STATIC_EXECUTOR_EVIDENCE_REFUSALS[args.operation]
            )
            dispatch_audit = (
                dispatch_trace.read_text(encoding="utf-8", errors="replace")
                if dispatch_trace.exists()
                else ""
            )
            preflight_marker = (
                f"preflight_observe op={operation_id} opcode={opcode} "
                f"code={opcode_code}"
            )
            dispatch_lines = tuple(
                line for line in dispatch_audit.splitlines() if line
            )
            refusal_prefix = (
                "layer=statement_context_dispatch_failure"
                "\tcode=SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING\t"
            )
            refusal_suffix = (
                "\taudit="
                + STATIC_EXECUTOR_EVIDENCE_AUDIT_KEYS.get(
                    args.operation,
                    "sblr.opcode_stream.member_preflight_refused",
                )
            )
            if (
                len(dispatch_lines) != 4
                or dispatch_lines[0] != preflight_marker
                or not dispatch_lines[1].startswith(refusal_prefix)
                or not dispatch_lines[1].endswith(refusal_suffix)
                or dispatch_lines[2] != preflight_marker
                or not dispatch_lines[3].startswith(refusal_prefix)
                or not dispatch_lines[3].endswith(refusal_suffix)
            ):
                raise ProofError(
                    f"{operation_label} did not produce exactly two ordered "
                    "authenticated canonical-SBLR static preflight/refusal pairs"
                )
            server_dispatch_audit = (
                server_trace.read_text(encoding="utf-8", errors="replace")
                if server_trace.exists()
                else ""
            )
            if server_dispatch_audit:
                raise ProofError(
                    f"{operation_label} refusal reached engine dispatch"
                )
            forbidden_markers = (
                "parent_success_barrier=passed",
                "result_descriptor_id=",
                "result_descriptor_version=",
                "executor_availability_generation=",
                "_result_sha256=",
                "layer=ddl_",
            )
            leaked_markers = tuple(
                marker for marker in forbidden_markers
                if marker in dispatch_audit
            )
            if leaked_markers:
                raise ProofError(
                    f"{operation_label} refusal leaked executor/result/"
                    f"publication evidence: {', '.join(leaked_markers)}"
                )
            if catalog_event_before != catalog_event_path.read_bytes():
                raise ProofError(
                    f"{operation_label} refusal mutated the canonical "
                    "catalog-object event journal"
                )
            executor_availability_after = executor_availability_snapshot()
            if executor_availability_before != executor_availability_after:
                changed_availability_paths = sorted(
                    set(executor_availability_before or {}) |
                    set(executor_availability_after)
                )
                raise ProofError(
                    f"{operation_label} refusal mutated executor-availability "
                    "state: " + ", ".join(changed_availability_paths)
                )
        if args.operation == "ddl-create-index":
            if verified.stderr:
                raise ProofError(
                    "independent CREATE INDEX refusal emitted unexpected stderr"
                )
            nonempty_phase_traces = tuple(
                str(path)
                for path in (server_trace, dispatch_trace)
                if path.exists() and path.stat().st_size != 0
            )
            if nonempty_phase_traces:
                raise ProofError(
                    "CREATE INDEX refusal reached executor/result/publication "
                    f"tracing: {', '.join(nonempty_phase_traces)}"
                )
            final_audit = "\n".join(
                path.read_text(encoding="utf-8", errors="replace")
                for path in audit_paths
                if path.exists()
            )
            forbidden_markers = (
                "engine.op.ddl_create_index",
                "SBLR_DDL_CREATE_INDEX",
                "opcode_code=1540",
                "create_index_descriptor",
                "ddl_create_index_result_sha256=",
            )
            leaked_markers = tuple(
                marker for marker in forbidden_markers if marker in final_audit
            )
            if leaked_markers:
                raise ProofError(
                    "CREATE INDEX refusal leaked canonical executor/result/"
                    f"publication evidence: {', '.join(leaked_markers)}"
                )
        if args.operation in (
            "ddl-create-type",
            "ddl-alter-type",
            "ddl-drop-type",
        ):
            if verified.stderr:
                raise ProofError(
                    "independent TYPE refusal emitted unexpected stderr"
                )
            nonempty_phase_traces = tuple(
                str(path)
                for path in (server_trace, dispatch_trace)
                if path.exists() and path.stat().st_size != 0
            )
            if nonempty_phase_traces:
                raise ProofError(
                    "TYPE refusal reached statement-context/executor/result "
                    f"tracing: {', '.join(nonempty_phase_traces)}"
                )
            final_audit = "\n".join(
                path.read_text(encoding="utf-8", errors="replace")
                for path in audit_paths
                if path.exists()
            )
            forbidden_markers = (
                "engine.op.ddl_create_type",
                "engine.op.ddl_alter_type",
                "engine.op.ddl_drop_type",
                "SBLR_DDL_CREATE_TYPE",
                "SBLR_DDL_ALTER_TYPE",
                "SBLR_DDL_DROP_TYPE",
                "create_type_descriptor",
                "alter_type_descriptor",
                "drop_type_descriptor",
            )
            leaked_markers = tuple(
                marker for marker in forbidden_markers if marker in final_audit
            )
            if leaked_markers:
                raise ProofError(
                    "TYPE refusal leaked canonical executor/result/publication "
                    f"evidence: {', '.join(leaked_markers)}"
                )
        print(f"sbsql_sblr_alignment_ia01_{args.operation.replace('-', '_')}_process_e2e=passed work={work}")
        return 0
    except Exception as exc:  # noqa: BLE001
        print(f"sbsql_sblr_alignment_ia01_{args.operation.replace('-', '_')}_process_e2e=failed work={work}: {exc}",
              file=sys.stderr)
        return 1
    finally:
        stop(server)


if __name__ == "__main__":
    raise SystemExit(main())
