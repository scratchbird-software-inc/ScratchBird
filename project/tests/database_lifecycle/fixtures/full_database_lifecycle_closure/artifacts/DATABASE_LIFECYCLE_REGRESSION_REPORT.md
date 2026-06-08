# Database Lifecycle Regression Report

Status: DBLC_P16_REGRESSION_COMPLETE evidence artifact
Search key: `DATABASE-LIFECYCLE-VALIDATION-PLAN`

This artifact is the DBLC-016/P16 exhaustive lifecycle regression report. The pass/fail authority is the CTest suite labeled `database_lifecycle_exhaustive` and the static gate labeled `DBLC_STATIC_REGRESSION_REPORT_ARTIFACT`; this report records the deterministic coverage contract those gates audit.

## Required Labels

Required lifecycle labels covered by generated CTest metadata and this report:

`database_lifecycle_unit`; `database_lifecycle_engine_api`; `database_lifecycle_storage`; `database_lifecycle_filespace`; `database_lifecycle_security`; `database_lifecycle_server_route`; `database_lifecycle_manager`; `database_lifecycle_listener`; `database_lifecycle_parser`; `database_lifecycle_process_association`; `database_lifecycle_server_daemon`; `database_lifecycle_ipc`; `database_lifecycle_session_request_cursor`; `database_lifecycle_engine_agent`; `database_lifecycle_cache_checkpoint`; `database_lifecycle_default_policy_catalog`; `database_lifecycle_default_policy_registry`; `database_lifecycle_policy_diagnostic_registry`; `database_lifecycle_policy_override_fixtures`; `database_lifecycle_config_policy_security_provider`; `database_lifecycle_backup_archive_restore`; `database_lifecycle_udr_extension`; `database_lifecycle_workload_resource`; `database_lifecycle_catalog_object`; `database_lifecycle_index_statistics_plan`; `database_lifecycle_lock_wait_deadlock`; `database_lifecycle_temp_workspace`; `database_lifecycle_event_notification`; `database_lifecycle_encryption_key`; `database_lifecycle_resource_seed`; `database_lifecycle_mga_gc_retention`; `database_lifecycle_jobs_scheduler`; `database_lifecycle_cluster_boundary`; `database_lifecycle_security_principal`; `database_lifecycle_storage_allocation`; `database_lifecycle_executable_object`; `database_lifecycle_sequence_generator`; `database_lifecycle_supportability_evidence`; `database_lifecycle_capability_profile`; `database_lifecycle_replication_boundary`; `database_lifecycle_existing_reconciliation`; `database_lifecycle_protocol_versioning`; `database_lifecycle_admin_cli`; `database_lifecycle_packaging_service`; `database_lifecycle_traceability`; `database_lifecycle_upgrade_migration`; `database_lifecycle_threat_model`; `database_lifecycle_shutdown_notification`; `database_lifecycle_catalog_index_profile`; `database_lifecycle_sys_information_projection`; `database_lifecycle_parser_route`; `database_lifecycle_donor_mapping`; `database_lifecycle_fault_injection`; `database_lifecycle_exhaustive`; `database_lifecycle_release`; `database_lifecycle_canonical_spec_closure`; `mga_transaction_regression`; `sbsql_parser_worker`.

The DBLC-016 gates also require `database_lifecycle`, `DBLC_P16_REGRESSION_COMPLETE`, `DBLC_STATIC_REGRESSION_REPORT_ARTIFACT`, `DBLC_P14_DONOR_MAPPING_COMPLETE`, `DBLC_P15_OBSERVABILITY_COMPLETE`, `sbsql_parser_worker`, and `mga_transaction_regression` evidence labels.

## Coverage Classes

| Coverage token | Evidence basis |
| --- | --- |
| `lifecycle_operation_core` | Create/open/attach/read/write/detach/maintenance/restricted-open/inspect/verify/repair/shutdown/reopen/drop operation labels are present. |
| `lifecycle_state_transition_core` | Startup, durable state, clean open, tx2 activation, maintenance, restricted open, shutdown, recovery, quarantine, and drop transition labels are present. |
| `lifecycle_invalid_transition_core` | Static and conformance gates cover stale, malformed, duplicate, missing, unsupported, unsafe, unauthorized, active-pin, and standalone-cluster refusal paths. |
| `lifecycle_route_core` | Engine API, public ABI, server route, IPC, manager, listener, parser, admin CLI, packaging, and SBSQL full-route labels are present. |
| `policy_override_no_override` | Default policy catalog and override fixture gates cover no-override behavior. |
| `policy_override_create_database_only` | Default policy registry and override fixture gates cover create-time-only overrides. |
| `policy_override_security_admin` | Policy override and security provider gates cover accepted and rejected security-admin overrides. |
| `policy_override_sysarch` | Policy override and security provider gates cover sysarch authority paths. |
| `policy_override_policy_defined` | Policy generation, stale-generation refusal, and policy-defined override classes are covered. |
| `policy_override_cluster_only` | Cluster-only policy requests fail closed in standalone mode through cluster-boundary and override fixtures. |
| `security_valid_credentials` | Attach/auth and SBSQL route gates cover valid credential admission. |
| `security_invalid_credentials` | Attach/auth, security principal, and message-vector gates cover invalid credential denial without session or transaction creation. |
| `auth_authority_engine_owned` | Engine-owned authentication labels and parser no-authority-bypass gates prove parser packages cannot authenticate or authorize as authority. |
| `resource_seed_epoch_coverage` | Resource seed, default policy, charset/collation/timezone, runtime epoch, and stale cache gates are present. |
| `diagnostic_message_vector_coverage` | Policy diagnostic registry, observability, admin CLI, threat model, and SBSQL message-vector gates are present. |
| `observability_metrics_audit_coverage` | Supportability evidence plus DBLC-015 observability labels cover metrics, audit, redaction, support bundles, and protected-material filtering. |
| `donor_mapping_firebird_sbsql` | Donor mapping labels and `DBLC_P14_DONOR_MAPPING_COMPLETE` cover FirebirdSQL and SBSQL donor mapping/emulation diagnostics. |
| `sbsql_full_route_coverage` | `database_lifecycle_parser_route`, SBSQL parser worker, SBWP/TLS route, full-route lifecycle, and server admission labels are present. |
| `mga_transaction_regression` | Existing MGA regression labels cover begin, commit, rollback, autocommit, savepoint, snapshot visibility, recovery classification, and finality authority. |
| `no_authoritative_wal_recovery` | MGA policy gates and SBSQL no-WAL/no-direct-database gates reject WAL as Alpha recovery authority. |
| `no_parser_finality_authority` | Parser lifecycle, observability, UDR, SBSQL stream finality, and MGA labels reject parser-side finality authority. |
| `no_donor_sql_execution` | Donor mapping static gates and parser worker replay gates reject donor SQL execution inside the engine boundary. |
| `evidence_report_present` | This report and `DATABASE_LIFECYCLE_REGRESSION_MATRIX.csv` are audited by the static gate. |

## Evidence Commands

Required DBLC-016 command:

```bash
ctest --test-dir build --output-on-failure -L database_lifecycle_exhaustive
```

Related invariant command:

```bash
ctest --test-dir build --output-on-failure -L mga_transaction_regression
```

The exhaustive tests inspect generated CTest labels from the build tree, this report, the regression matrix, `VALIDATION_PLAN.md`, and the existing DBLC-016 gate rows without editing tracker CSVs or acceptance gate CSVs.

## Validation Snapshot

Local validation on 2026-05-10:

| Command | Result |
| --- | --- |
| `cmake -S project -B build` | Passed. |
| `cmake --build build --target database_lifecycle_exhaustive_regression_conformance -j2` | Passed. |
| `ctest --test-dir build --output-on-failure -L database_lifecycle_exhaustive` | Passed: 2 tests passed and 0 failed. |
| `ctest --test-dir build --output-on-failure -L mga_transaction_regression` | Passed: 49 tests passed and 0 failed after authority drift correction for cross-session cancel. |
| `python3 ${PUBLIC_TOOL_ROOT}/skills/scratchbird-mga-transaction-authority/scripts/mga_policy_gate.py` | Passed: `mga_policy_gate=passed`. |
