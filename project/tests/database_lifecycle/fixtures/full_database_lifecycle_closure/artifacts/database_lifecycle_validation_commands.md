# Database Lifecycle Validation Commands

Status: draft/preparatory
Search key: `DATABASE-LIFECYCLE-VALIDATION-COMMANDS`

This file materializes validation commands for DBLC-000C before lifecycle implementation starts. DBLC-000C is not closed by this draft: DBLC-000B remains a prerequisite and coordinator validation is required before P0C can be marked passed.

Run commands from the repository root:

```bash
cd ${PROJECT_ROOT}
```

The required slice-to-gate matrix is `artifacts/DATABASE_LIFECYCLE_CTEST_REQUIRED_GATES.csv`. CTest commands use labels from `VALIDATION_PLAN.md` where a label exists. Slices without an available CTest label use explicit static or audit command references below.

## Prerequisite Gate

DBLC-000C validation is preparatory until DBLC-000B closes.

```bash
# static:DBLC_STATIC_P0C_PREREQUISITE
test -s project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/artifacts/DATABASE_LIFECYCLE_IMPLEMENTATION_GAP_MATRIX.csv
```

## Required CTest Label Commands

These commands are copied into executable form from `VALIDATION_PLAN.md`.

```bash
ctest --test-dir build --output-on-failure -L database_lifecycle_unit
ctest --test-dir build --output-on-failure -L database_lifecycle_engine_api
ctest --test-dir build --output-on-failure -L database_lifecycle_storage
ctest --test-dir build --output-on-failure -L database_lifecycle_filespace
ctest --test-dir build --output-on-failure -L database_lifecycle_security
ctest --test-dir build --output-on-failure -L database_lifecycle_server_route
ctest --test-dir build --output-on-failure -L database_lifecycle_manager
ctest --test-dir build --output-on-failure -L database_lifecycle_listener
ctest --test-dir build --output-on-failure -L database_lifecycle_parser
ctest --test-dir build --output-on-failure -L database_lifecycle_process_association
ctest --test-dir build --output-on-failure -L database_lifecycle_server_daemon
ctest --test-dir build --output-on-failure -L database_lifecycle_ipc
ctest --test-dir build --output-on-failure -L database_lifecycle_session_request_cursor
ctest --test-dir build --output-on-failure -L database_lifecycle_engine_agent
ctest --test-dir build --output-on-failure -L database_lifecycle_cache_checkpoint
ctest --test-dir build --output-on-failure -L database_lifecycle_default_policy_catalog
ctest --test-dir build --output-on-failure -L database_lifecycle_default_policy_registry
ctest --test-dir build --output-on-failure -L database_lifecycle_policy_diagnostic_registry
ctest --test-dir build --output-on-failure -L database_lifecycle_policy_override_fixtures
ctest --test-dir build --output-on-failure -L database_lifecycle_canonical_spec_closure
ctest --test-dir build --output-on-failure -L database_lifecycle_config_policy_security_provider
ctest --test-dir build --output-on-failure -L database_lifecycle_backup_archive_restore
ctest --test-dir build --output-on-failure -L database_lifecycle_udr_extension
ctest --test-dir build --output-on-failure -L database_lifecycle_workload_resource
ctest --test-dir build --output-on-failure -L database_lifecycle_catalog_object
ctest --test-dir build --output-on-failure -L database_lifecycle_index_statistics_plan
ctest --test-dir build --output-on-failure -L database_lifecycle_lock_wait_deadlock
ctest --test-dir build --output-on-failure -L database_lifecycle_temp_workspace
ctest --test-dir build --output-on-failure -L database_lifecycle_event_notification
ctest --test-dir build --output-on-failure -L database_lifecycle_encryption_key
ctest --test-dir build --output-on-failure -L database_lifecycle_resource_seed
ctest --test-dir build --output-on-failure -L database_lifecycle_mga_gc_retention
ctest --test-dir build --output-on-failure -L database_lifecycle_jobs_scheduler
ctest --test-dir build --output-on-failure -L database_lifecycle_cluster_boundary
ctest --test-dir build --output-on-failure -L database_lifecycle_security_principal
ctest --test-dir build --output-on-failure -L database_lifecycle_storage_allocation
ctest --test-dir build --output-on-failure -L database_lifecycle_executable_object
ctest --test-dir build --output-on-failure -L database_lifecycle_sequence_generator
ctest --test-dir build --output-on-failure -L database_lifecycle_supportability_evidence
ctest --test-dir build --output-on-failure -L database_lifecycle_capability_profile
ctest --test-dir build --output-on-failure -L database_lifecycle_replication_boundary
ctest --test-dir build --output-on-failure -L database_lifecycle_existing_reconciliation
ctest --test-dir build --output-on-failure -L database_lifecycle_protocol_versioning
ctest --test-dir build --output-on-failure -L database_lifecycle_admin_cli
ctest --test-dir build --output-on-failure -L database_lifecycle_packaging_service
ctest --test-dir build --output-on-failure -L database_lifecycle_traceability
ctest --test-dir build --output-on-failure -L database_lifecycle_upgrade_migration
ctest --test-dir build --output-on-failure -L database_lifecycle_threat_model
ctest --test-dir build --output-on-failure -L database_lifecycle_shutdown_notification
ctest --test-dir build --output-on-failure -L database_lifecycle_catalog_index_profile
ctest --test-dir build --output-on-failure -L database_lifecycle_sys_information_projection
ctest --test-dir build --output-on-failure -L database_lifecycle_parser_route
ctest --test-dir build --output-on-failure -L database_lifecycle_reference_mapping
ctest --test-dir build --output-on-failure -L database_lifecycle_fault_injection
ctest --test-dir build --output-on-failure -L database_lifecycle_exhaustive
ctest --test-dir build --output-on-failure -L database_lifecycle_release
ctest --test-dir build --output-on-failure -L mga_transaction_regression
ctest --test-dir build --output-on-failure -L sbsql_parser_worker
```

## Static And Audit Commands

```bash
# static:DBLC_STATIC_AUTHORITY_ARTIFACTS
test -s project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/artifacts/DATABASE_LIFECYCLE_AUTHORITY_MAP.csv && test -s project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/artifacts/DATABASE_LIFECYCLE_BASELINE_INVENTORY.md

# static:DBLC_STATIC_STATE_NO_DEFER_ARTIFACTS
test -s project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/artifacts/DATABASE_LIFECYCLE_STATE_MODEL_DRAFT.md && test -s project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/artifacts/NO_DEFER_LIFECYCLE_CONTRACT.md

# static:DBLC_STATIC_GAP_MATRIX_ARTIFACT
test -s project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/artifacts/DATABASE_LIFECYCLE_IMPLEMENTATION_GAP_MATRIX.csv && rg -q 'diagnostic|registry|parser|server|test' project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/artifacts/DATABASE_LIFECYCLE_IMPLEMENTATION_GAP_MATRIX.csv

# static:DBLC_STATIC_VALIDATION_ARTIFACT_COVERAGE
test -s project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/artifacts/DATABASE_LIFECYCLE_CTEST_REQUIRED_GATES.csv && test -s project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/artifacts/database_lifecycle_validation_commands.md && awk -F, 'NR==FNR && FNR>1 {want[$1]=1; next} NR!=FNR && FNR>1 {have[$1]=1} END {for (s in want) if (!(s in have)) {print "missing validation gate for " s; bad=1} exit bad}' project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/TRACKER.csv project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/artifacts/DATABASE_LIFECYCLE_CTEST_REQUIRED_GATES.csv

# static:DBLC_STATIC_AGENT_ORCHESTRATION_ARTIFACTS
test -s project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/artifacts/DATABASE_LIFECYCLE_AGENT_ORCHESTRATION.md && test -s project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/artifacts/DATABASE_LIFECYCLE_AGENT_STATUS.csv && test -s project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/artifacts/DATABASE_LIFECYCLE_AGENT_WRITE_SCOPE_REGISTER.csv && test -s project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/artifacts/DATABASE_LIFECYCLE_AGENT_HEARTBEAT_LOG.csv && test -s project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/artifacts/DATABASE_LIFECYCLE_AGENT_FAILURE_INVENTORY.csv && test -s project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/artifacts/DATABASE_LIFECYCLE_SLICE_EXECUTION_QUEUE.csv

# static:DBLC_STATIC_DEFAULT_POLICY_SINGLE_AUTHORITY
test -s public_input_snapshot && rg -q 'default policy|policy family|override' public_input_snapshot

# static:DBLC_STATIC_POLICY_REGISTRY_HYGIENE
test -s public_contract_snapshot && test -s public_contract_snapshot && test -s public_contract_snapshot && ! git check-ignore -q public_contract_snapshot public_contract_snapshot

# static:DBLC_STATIC_DEFAULT_POLICY_NO_HARDCODED_VALUES
test -d project/src && ! rg -n 'default policy|hardcoded default|implicit default' project/src project/tests --glob '!**/default-policy-catalog.yaml'

# static:DBLC_STATIC_CANONICAL_SPEC_CLOSURE
test -s public_input_snapshot && rg -q 'create|open|attach|detach|maintenance|restricted|diagnostic|repair|recovery|shutdown|drop' public_input_snapshot

# static:DBLC_STATIC_LIFECYCLE_REGISTRY_SURFACE
test -s project/src/engine/internal_api/ENGINE_API_SURFACE_REGISTRY.yaml && test -s project/src/engine/internal_api/SBLR_API_OPERATION_MATRIX.yaml && rg -q 'lifecycle|database' project/src/engine/internal_api/ENGINE_API_SURFACE_REGISTRY.yaml project/src/engine/internal_api/SBLR_API_OPERATION_MATRIX.yaml

# static:DBLC_STATIC_MGA_FINALITY_AUTHORITY
test -d project/src && ! rg -n 'WAL.*final|checkpoint.*finality|cache.*transaction finality|backup.*transaction finality' project/src public_contract_snapshot

# static:DBLC_STATIC_FILESPACE_UUIDV7_IDENTITY
rg -q 'filespace.*UUIDv7|UUIDv7.*filespace' public_release_evidence project/src project/tests

# static:DBLC_STATIC_CATALOG_IDENTITY_BOUNDARY
rg -q 'identity resolver|sys.information|sys.catalog' public_contract_snapshot public_contract_snapshot

# static:DBLC_STATIC_CATALOG_INDEX_PROFILE_BOUNDARY
rg -q 'hash index|B-tree|UUID exact|generation|history' public_contract_snapshot public_contract_snapshot

# static:DBLC_STATIC_NO_LEGACY_LIFECYCLE_DRIFT
test -d project/src && ! rg -n 'legacy lifecycle|old lifecycle|temporary lifecycle assumption|compatibility shortcut' project/src project/tests

# static:DBLC_STATIC_PROTOCOL_VERSIONING
rg -q 'version|upgrade|downgrade|compatibility|fail closed' public_contract_snapshot project/src

# static:DBLC_STATIC_ADMIN_AUTH_AUDIT_ROUTE
rg -q 'authorization|audit|message vector|route admission|engine authority' public_contract_snapshot public_contract_snapshot project/src

# static:DBLC_STATIC_RUNTIME_CLEANUP
rg -q 'runtime directory|PID|owner file|socket|control endpoint|cleanup' public_contract_snapshot public_contract_snapshot project/src

# static:DBLC_STATIC_MIGRATION_NO_GUESSING
test -d project/src && ! rg -n 'guess.*identity|guess.*generation|guess.*format|guess.*policy|guess.*transaction outcome' public_release_evidence project/src project/tests

# static:DBLC_STATIC_THREAT_MODEL_ABUSE_CASES
rg -q 'abuse|threat|fail closed|least privilege' public_release_evidence project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure project/tests

# static:DBLC_STATIC_TRACEABILITY_COVERAGE
test -s project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/TRACKER.csv && test -s project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/VALIDATION_PLAN.md && test -s project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/artifacts/DATABASE_LIFECYCLE_CTEST_REQUIRED_GATES.csv

# static:DBLC_STATIC_NO_REFERENCE_ENGINE_SQL
test -d project/src && ! rg -n 'reference.*execute.*SQL|execute.*SQL.*engine boundary|reference.*engine.*SQL' project/src/parsers project/src/udr public_contract_snapshot

# static:DBLC_STATIC_DIAGNOSTIC_MESSAGE_VECTOR_AUDIT
rg -q 'message vector|diagnostic|audit|metrics|cache invalidation' public_release_evidence project/src project/tests

# static:DBLC_STATIC_REGRESSION_REPORT_ARTIFACT
test -s project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/artifacts/DATABASE_LIFECYCLE_REGRESSION_REPORT.md

# static:DBLC_STATIC_AUTHORITY_DRIFT_GATES
ctest --test-dir build --output-on-failure -L database_lifecycle_fault_injection && ctest --test-dir build --output-on-failure -L mga_transaction_regression

# static:DBLC_STATIC_NO_LIFECYCLE_PLACEHOLDERS
test -d project/src && ! rg -n -i 'TODO|TBD|stub|placeholder|defer|deferred|future work' project/src/storage project/src/server project/src/listener project/src/parsers project/tests/database_lifecycle

# static:DBLC_STATIC_FINAL_ZERO_OPEN_AUDIT
test -s project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/FINAL_AUDIT.md && test -s project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/VALIDATION_RESULT.md && test -s project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/CLOSURE_REPORT.md && test -s project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/artifacts/final_database_lifecycle_zero_open_audit.py
```

## Slice Command References

| Slice | Required command refs |
| --- | --- |
| DBLC-000 | `static:DBLC_STATIC_AUTHORITY_ARTIFACTS` |
| DBLC-000A | `static:DBLC_STATIC_STATE_NO_DEFER_ARTIFACTS` |
| DBLC-000B | `static:DBLC_STATIC_GAP_MATRIX_ARTIFACT` |
| DBLC-000C | `static:DBLC_STATIC_P0C_PREREQUISITE`, `static:DBLC_STATIC_VALIDATION_ARTIFACT_COVERAGE` |
| DBLC-000D | `static:DBLC_STATIC_AGENT_ORCHESTRATION_ARTIFACTS` |
| DBLC-000E | `ctest:database_lifecycle_default_policy_catalog`, `static:DBLC_STATIC_DEFAULT_POLICY_SINGLE_AUTHORITY` |
| DBLC-000F | `ctest:database_lifecycle_default_policy_registry`, `ctest:database_lifecycle_policy_diagnostic_registry`, `ctest:database_lifecycle_policy_override_fixtures`, `static:DBLC_STATIC_POLICY_REGISTRY_HYGIENE`, `static:DBLC_STATIC_DEFAULT_POLICY_NO_HARDCODED_VALUES` |
| DBLC-001 | `ctest:database_lifecycle_canonical_spec_closure`, `static:DBLC_STATIC_CANONICAL_SPEC_CLOSURE` |
| DBLC-002 | `ctest:database_lifecycle_engine_api`, `static:DBLC_STATIC_LIFECYCLE_REGISTRY_SURFACE` |
| DBLC-003 | `ctest:database_lifecycle_unit`, `ctest:database_lifecycle_storage`, `ctest:mga_transaction_regression`, `static:DBLC_STATIC_MGA_FINALITY_AUTHORITY` |
| DBLC-004 | `ctest:database_lifecycle_storage`, `ctest:database_lifecycle_default_policy_catalog`, `static:DBLC_STATIC_DEFAULT_POLICY_SINGLE_AUTHORITY` |
| DBLC-004A | `ctest:database_lifecycle_filespace`, `static:DBLC_STATIC_FILESPACE_UUIDV7_IDENTITY` |
| DBLC-005 | `ctest:database_lifecycle_storage` |
| DBLC-006 | `ctest:database_lifecycle_storage` |
| DBLC-007 | `ctest:database_lifecycle_security` |
| DBLC-008 | `ctest:database_lifecycle_storage`, `ctest:database_lifecycle_security`, `ctest:database_lifecycle_cluster_boundary` |
| DBLC-009 | `ctest:database_lifecycle_server_route`, `ctest:database_lifecycle_ipc`, `ctest:database_lifecycle_session_request_cursor`, `ctest:database_lifecycle_parser` |
| DBLC-010 | `ctest:database_lifecycle_storage`, `ctest:database_lifecycle_admin_cli` |
| DBLC-011 | `ctest:database_lifecycle_server_route`, `ctest:database_lifecycle_shutdown_notification`, `ctest:database_lifecycle_process_association`, `ctest:database_lifecycle_storage` |
| DBLC-012 | `ctest:database_lifecycle_storage`, `ctest:database_lifecycle_filespace` |
| DBLC-013 | `ctest:database_lifecycle_server_route`, `ctest:database_lifecycle_parser_route`, `ctest:sbsql_parser_worker` |
| DBLC-013A | `ctest:database_lifecycle_manager` |
| DBLC-013B | `ctest:database_lifecycle_listener` |
| DBLC-013C | `ctest:database_lifecycle_parser` |
| DBLC-013D | `ctest:database_lifecycle_process_association` |
| DBLC-013E | `ctest:database_lifecycle_server_daemon` |
| DBLC-013F | `ctest:database_lifecycle_ipc` |
| DBLC-013G | `ctest:database_lifecycle_session_request_cursor` |
| DBLC-013H | `ctest:database_lifecycle_engine_agent` |
| DBLC-013I | `ctest:database_lifecycle_cache_checkpoint` |
| DBLC-013J | `ctest:database_lifecycle_config_policy_security_provider`, `ctest:database_lifecycle_default_policy_catalog` |
| DBLC-013K | `ctest:database_lifecycle_backup_archive_restore` |
| DBLC-013L | `ctest:database_lifecycle_udr_extension` |
| DBLC-013M | `ctest:database_lifecycle_workload_resource` |
| DBLC-013U | `ctest:database_lifecycle_catalog_object` |
| DBLC-013U1 | `ctest:database_lifecycle_catalog_index_profile`, `ctest:database_lifecycle_sys_information_projection`, `ctest:database_lifecycle_catalog_object`, `static:DBLC_STATIC_CATALOG_IDENTITY_BOUNDARY`, `static:DBLC_STATIC_CATALOG_INDEX_PROFILE_BOUNDARY` |
| DBLC-013AA | `ctest:database_lifecycle_resource_seed` |
| DBLC-013V | `ctest:database_lifecycle_index_statistics_plan` |
| DBLC-013W | `ctest:database_lifecycle_lock_wait_deadlock` |
| DBLC-013X | `ctest:database_lifecycle_temp_workspace` |
| DBLC-013Y | `ctest:database_lifecycle_event_notification` |
| DBLC-013Z | `ctest:database_lifecycle_encryption_key` |
| DBLC-013AB | `ctest:database_lifecycle_mga_gc_retention` |
| DBLC-013AC | `ctest:database_lifecycle_jobs_scheduler` |
| DBLC-013AD | `ctest:database_lifecycle_cluster_boundary` |
| DBLC-013AE | `ctest:database_lifecycle_security_principal` |
| DBLC-013AF | `ctest:database_lifecycle_storage_allocation` |
| DBLC-013AG | `ctest:database_lifecycle_executable_object` |
| DBLC-013AH | `ctest:database_lifecycle_sequence_generator` |
| DBLC-013AI | `ctest:database_lifecycle_supportability_evidence` |
| DBLC-013AJ | `ctest:database_lifecycle_capability_profile` |
| DBLC-013AK | `ctest:database_lifecycle_replication_boundary` |
| DBLC-013N | `ctest:database_lifecycle_existing_reconciliation`, `static:DBLC_STATIC_NO_LEGACY_LIFECYCLE_DRIFT` |
| DBLC-013O | `ctest:database_lifecycle_protocol_versioning`, `static:DBLC_STATIC_PROTOCOL_VERSIONING` |
| DBLC-013P | `ctest:database_lifecycle_admin_cli`, `static:DBLC_STATIC_ADMIN_AUTH_AUDIT_ROUTE` |
| DBLC-013Q | `ctest:database_lifecycle_packaging_service`, `static:DBLC_STATIC_RUNTIME_CLEANUP` |
| DBLC-013S | `ctest:database_lifecycle_upgrade_migration`, `static:DBLC_STATIC_MIGRATION_NO_GUESSING` |
| DBLC-013T | `ctest:database_lifecycle_threat_model`, `static:DBLC_STATIC_THREAT_MODEL_ABUSE_CASES` |
| DBLC-013R | `ctest:database_lifecycle_traceability`, `static:DBLC_STATIC_TRACEABILITY_COVERAGE` |
| DBLC-014 | `ctest:database_lifecycle_reference_mapping`, `ctest:database_lifecycle_parser_route`, `ctest:sbsql_parser_worker`, `static:DBLC_STATIC_NO_REFERENCE_ENGINE_SQL` |
| DBLC-015 | `ctest:database_lifecycle_supportability_evidence`, `static:DBLC_STATIC_DIAGNOSTIC_MESSAGE_VECTOR_AUDIT` |
| DBLC-016 | `ctest:database_lifecycle_exhaustive`, `static:DBLC_STATIC_REGRESSION_REPORT_ARTIFACT` |
| DBLC-017 | `ctest:database_lifecycle_fault_injection`, `ctest:mga_transaction_regression`, `static:DBLC_STATIC_AUTHORITY_DRIFT_GATES`, `static:DBLC_STATIC_NO_LIFECYCLE_PLACEHOLDERS` |
| DBLC-018 | `ctest:database_lifecycle_release`, `static:DBLC_STATIC_FINAL_ZERO_OPEN_AUDIT` |
