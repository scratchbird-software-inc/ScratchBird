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

from ia01_package_process_e2e import ProofError, allocate_work, seed_database, stop, wait_path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--client", required=True)
    parser.add_argument("--operation", choices=("show-version", "show-wait-events", "show-object-detail", "source-map", "error-vector", "txn-begin", "txn-commit", "txn-rollback", "txn-savepoint", "txn-release-savepoint", "psql-autonomous-frame", "transaction-reservation-release", "temporary-instance-cleanup", "cursor-open", "cursor-fetch", "cursor-close", "read-by-key", "read-range", "read-stream", "result-set-pass", "access-cursor-open", "access-cursor-fetch", "access-cursor-close", "insert", "update", "delete", "merge", "table-truncate", "table-analyze", "bulk-import-stream", "bulk-export-stream", "statement-batch", "atomic-cas", "atomic-rmw", "advisory-lock", "advisory-lock-release", "function-call", "operator-call", "cast", "compare", "domain-operation", "udr-invoke", "procedure-invoke", "function-invoke", "aggregate-invoke", "sequence-nextval", "sequence-currval", "sequence-setval", "query-numeric", "advanced-datatype-family", "project", "aggregate", "group", "sort", "limit", "window", "return-result-set", "kv-structured-read", "kv-structured-mutate", "kv-structured-scan", "kv-structured-stream-read", "kv-structured-stream-append", "kv-structured-timeseries", "system-config-set", "ddl-create-domain", "ddl-alter-domain", "ddl-create-view", "ddl-alter-view", "ddl-drop-view", "ddl-create-trigger", "ddl-create-schema", "ddl-create-table", "ddl-create-index", "ddl-drop-index"),
                        default="source-map")
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "system-config-get", "system-config-reset", "ddl-create-rule", "ddl-drop-rule", "ddl-create-publication", "ddl-alter-publication", "ddl-drop-publication", "ddl-create-subscription", "ddl-alter-subscription", "ddl-drop-subscription", "ddl-create-operator", "ddl-drop-operator", "ddl-create-operator-class", "ddl-drop-operator-class", "ddl-create-operator-family", "ddl-alter-operator-family", "ddl-drop-cast", "ddl-create-extension", "ddl-alter-extension", "ddl-drop-extension"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "ddl-alter-trigger", "ddl-refresh-materialized-view", "ddl-create-materialized-view", "ddl-drop-materialized-view", "ddl-drop-package", "ddl-drop-synonym", "ddl-drop-foreign-table", "ddl-alter-package", "ddl-alter-sequence", "ddl-drop-sequence", "ddl-create-type", "ddl-alter-type", "ddl-drop-type", "ddl-drop-table", "ddl-create-table-as-query-with-data", "ddl-create-table-as-query-with-no-data", "ddl-create-sequence"))
    parser._actions[-1].choices = tuple((*parser._actions[-1].choices, "dml-counter-add"))
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
    parser.add_argument("--work-dir", required=True)
    args = parser.parse_args()
    work = allocate_work(Path(args.work_dir))
    server = None
    try:
        database = work / "source_map.sbdb"
        endpoint = work / "sc" / "s.sock"
        evidence = seed_database(Path(args.server), database)
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
        wait_path(endpoint)
        command = [args.client, f"unix:{endpoint}", str(database), "alice",
                   evidence, args.operation, f"sbsql-sblr-{args.operation}-e2e-first"]
        first = subprocess.run(command, capture_output=True, text=True, timeout=30)
        if first.returncode != 0:
            raise ProofError(f"explicit source-map operation failed: {first.stdout}{first.stderr}")
        audit_paths = (server_trace, dispatch_trace,
                       work / "sc" / "sb_server.audit.jsonl")
        audit = "\n".join(p.read_text(encoding="utf-8", errors="replace")
                          for p in audit_paths if p.exists())
        if args.operation == "show-version":
            expected = ("operation_id=observability.show_version",
                        "opcode=SBLR_OBSERVABILITY_SHOW_VERSION",
                        "opcode_code=3334")
        elif args.operation == "show-wait-events":
            expected = ("executor_id=engine.op.read_metrics", "opcode=SBLR_READ_METRICS",
                        "opcode_code=3073", "request_sha256=")
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
        elif args.operation == "ddl-drop-cast":
            expected = ()
        elif args.operation == "ddl-create-extension":
            expected = ()
        elif args.operation == "ddl-alter-extension":
            expected = ()
        elif args.operation == "ddl-drop-extension":
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
        elif args.operation == "ddl-alter-rewrite-rule":
            expected = ("executor_id=engine.op.ddl_alter_rewrite_rule", "opcode=SBLR_DDL_ALTER_REWRITE_RULE", "opcode_code=1618", "operand_descriptor_id=rewrite_rule_alter_descriptor", "result_descriptor_id=ddl_result", "result_descriptor_version=1", "ddl_alter_rewrite_rule_result_sha256=", "executor_availability_generation=")
        elif args.operation == "ddl-drop-rewrite-rule":
            expected = ("executor_id=engine.op.ddl_drop_rewrite_rule", "opcode=SBLR_DDL_DROP_REWRITE_RULE", "opcode_code=1619", "operand_descriptor_id=rewrite_rule_drop_descriptor", "result_descriptor_id=ddl_result", "result_descriptor_version=1", "executor_availability_generation=")
        elif args.operation == "ddl-validate-constraint":
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
        elif args.operation == "dml-timeseries-schema-write":
            expected = ()
        elif args.operation == "alter-gpu-profile-disable":
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
            expected = ("executor_id=engine.op.ddl_create_index", "opcode=SBLR_DDL_CREATE_INDEX", "opcode_code=1540", "operand_descriptor_id=create_index_descriptor", "result_descriptor_id=ddl_result", "result_descriptor_version=1", "ddl_create_index_result_sha256=", "executor_availability_generation=")
        elif args.operation == "ddl-drop-index":
            expected = ("executor_id=engine.op.ddl_drop_index", "opcode=SBLR_DDL_DROP_INDEX", "opcode_code=1541", "operand_descriptor_id=drop_index_descriptor", "result_descriptor_id=ddl_result", "result_descriptor_version=1", "ddl_drop_index_result_sha256=", "executor_availability_generation=")
        else:
            expected = ("executor_id=engine.op.txn_begin", "opcode=SBLR_TXN_BEGIN",
                        "opcode_code=256", "operand_descriptor_id=transaction_begin_options",
                        "result_descriptor_id=transaction_handle",
                        "result_descriptor_version=1", "transaction_handle_sha256=",
                        "executor_availability_generation=")
        required_markers = (*expected, "parent_success_barrier=passed")
        if args.operation in ("ddl-create-operator-class", "ddl-drop-operator-class", "ddl-create-operator-family", "ddl-alter-operator-family", "ddl-drop-cast", "ddl-create-extension", "ddl-alter-extension", "ddl-drop-extension"):
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
