# Public Admin Runbooks

PUBLIC_ADMIN_RUNBOOKS

Authority: public_release_evidence_only.

These runbooks describe first-release public operation evidence. They do not define engine behavior.
The engine executes SBLR and internal procedures only; SQL text is not runtime authority.
MGA transaction inventory remains finality authority for transaction visibility, rollback, recovery, sweep, archive, and shutdown outcomes.
Cluster-positive production behavior is outside the public runbook scope unless an external provider is explicitly present.

## Coverage Matrix

| Runbook key | Operator topic | Public evidence anchors |
| --- | --- | --- |
| RUNBOOK_CREATE_DATABASE | Create a public example database from approved public seed inputs; reject overwrite and malformed create state with stable diagnostics. | `project/tools/release/public_example_database_seed.cpp`; `public_example_database_seed_gate`; `CreateDatabaseFile` |
| RUNBOOK_OPEN_DATABASE | Open the created database through public storage lifecycle evidence and report stable diagnostics for invalid open state. | `project/tools/release/public_example_database_seed.cpp`; `public_example_database_seed_gate`; `OpenDatabaseFile` |
| RUNBOOK_CONFIG_DEFAULTS | Check production defaults, service mode, secure paths, memory policy, startup files, and fail-closed unsafe runtime permissions. | `public_default_config_check`; `public_secure_defaults_gate`; `PUBLIC_DEFAULT_CONFIG_CHECK`; `PUBLIC_SECURE_DEFAULTS_GATE` |
| RUNBOOK_SECURITY_POLICY | Validate policy-pack selection, default policy coverage, provider refusal, protected material handling, and enterprise abuse-case diagnostics. | `public_policy_coverage_matrix_gate`; `public_custom_policy_pack_gate`; `public_enterprise_threat_gate`; `SECURITY.AUTH_PROVIDER_UNSUPPORTED` |
| RUNBOOK_BACKUP | Run public backup coverage and write-after backup-forward checks without treating backup records as transaction finality authority. | `public_backup_forward_session_gate`; `public_backup_update_coverage_gate`; `backup_forward`; `write_after` |
| RUNBOOK_RESTORE | Restore public backup evidence, reuse coverage, idempotency, PITR checks, and identity handling through public restore gates. | `public_backup_update_coverage_gate`; `public_cluster_catalog_backup_export_gate`; `restore`; `identity_remap` |
| RUNBOOK_VERIFY | Verify public examples, offline disk-resource bundles, and backup/restore evidence with stable diagnostics. | `public_example_smoke_gate`; `public_disk_resource_bundle_gate`; `--verify`; `diagnostics` |
| RUNBOOK_REPAIR | Inspect repair event ledgers, history, identity rules, tamper evidence, retention, legal hold, and crash-resume behavior. | `public_repair_event_ledger_quarantine_gate`; `public_repair_history_inspection_api_gate`; `public_repair_identity_rules_gate`; `public_repair_tamper_retention_crash_resume_gate` |
| RUNBOOK_MEMORY_PRESSURE | Diagnose memory pressure through default manager, reservation, executor pressure, PMR call path, and capacity baseline evidence. | `public_memory_pressure_executor_gate`; `public_query_memory_reservation_gate`; `public_performance_baseline_gate`; `memory_pressure` |
| RUNBOOK_SWEEP | Run MGA physical sweep evidence for free-space and index cleanup while preserving MGA authority. | `public_mga_physical_sweep_gate`; `SB-ROW-DATA-PHYSICAL-SWEEP-MGA-AUTHORITY-REQUIRED`; `free_space`; `index_cleanup` |
| RUNBOOK_ARCHIVE | Run archive-before-reclaim, backup-forward, multi-horizon disposal, and archive capacity evidence without cluster overclaim. | `public_archive_before_reclaim_gate`; `public_backup_forward_session_gate`; `public_performance_baseline_gate`; `archive_slice` |
| RUNBOOK_DIAGNOSTICS | Generate stable public diagnostic matrices with redaction classes, compatibility status, source paths, and public test paths. | `public_diagnostic_stability_gate`; `PUBLIC_DIAGNOSTIC_MATRIX_GENERATOR`; `stable_public`; `fail_closed_stable` |
| RUNBOOK_UNSUPPORTED_FEATURES | Report unsupported features through deterministic fail-closed diagnostics without runtime execution or authority overclaim. | `public_unsupported_feature_gate`; `PUBLIC_UNSUPPORTED_FEATURE_MATRIX`; `external_provider_required`; `compile_time_disabled`; `policy_blocked` |
| RUNBOOK_UPGRADE | Run ODF, catalog page, index, datatype, policy-pack, and cluster-catalog migration checks, including interrupted upgrade rollback and downgrade refusal. | `public_upgrade_migration_gate`; `PUBLIC_UPGRADE_MIGRATION_GATE`; `downgrade_refusal`; `rollback` |

## Operator Boundaries

Public runbook output is support and release evidence. It is not storage,
transaction, security, optimizer, cluster, or agent authority. A runbook may
point to public gates and generated matrices, but a successful runbook check is
never a substitute for durable MGA transaction inventory, authorization policy
state, or engine-owned recovery classification.
