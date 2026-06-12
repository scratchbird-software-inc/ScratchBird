# Engine And Listener Enterprise Release Guide

ENGINE_LISTENER_ENTERPRISE_DOCS

Authority: public_release_evidence_only.

This guide is the public enterprise operating and architecture documentation
for the engine and listener release surface. It does not define runtime
authority. The engine executes admitted SBLR and internal APIs only.
Parser output is translation evidence until the listener and engine accept it.
MGA transaction inventory remains transaction finality authority.
Durable authorization state remains authorization authority.
Indexes and optimizer evidence are never final row authority.
Cluster-positive execution is outside the public core unless a compile-time
admitted external provider is present.

## Documentation Status Terms

The documentation set uses these exact support-state terms:

- production-supported: the behavior is implemented and backed by public CTest
  proof for the active platform lane.
- provider-required: the behavior requires an external provider before positive
  execution is supported.
- cluster-provider-only: the public core exposes only the compile/link provider
  boundary or no-cluster refusal path.
- disabled-by-default: the behavior exists only behind explicit configuration
  or compile-time admission.
- diagnostic-only: the surface reports state or failure details and does not
  grant runtime authority.
- evidence-only: the surface records proof and support data and does not change
  engine state.
- experimental: the behavior is not a public enterprise support promise.
- unsupported: the behavior must fail closed with stable diagnostics.

## DOC_ARCHITECTURE_OVERVIEW

The public product is split into listener, parser-worker boundary, server
management route, and engine execution route.

- Listener responsibility: accept connections, bind DBBT/LPREFACE identity,
  hand off parser workers, expose authenticated management, drain, stop, and
  emit listener support evidence.
- Parser-worker responsibility: translate input protocol or dialect into SBLR
  and associated evidence. Parser output is not engine authority.
- Engine responsibility: authenticate sessions, materialize authorization,
  admit SBLR envelopes, execute internal APIs, maintain durable storage, and
  publish transaction-visible results.

Public proof anchors: `engine_listener_integrated_product_proof_gate`,
`engine_listener_operational_readiness_gate`, `public_api_boundary_gate`.

## DOC_TRUST_BOUNDARY_MODEL

Untrusted inputs include client frames, parser-worker messages, management
commands before authentication, SBLR envelopes before admission, database files
before open validation, support-bundle requests, and cluster-provider claims.

Trusted authority is intentionally narrow:

- engine-owned durable catalogs for database identity, security policy, and
  metadata;
- MGA transaction inventory for visibility, rollback, recovery, cleanup, and
  shutdown finality;
- materialized authorization state for access decisions;
- validated SBLR envelope identity for engine dispatch;
- compile-time cluster provider admission for cluster-positive routes.

Public proof anchors: `engine_listener_adversarial_security_validation_gate`,
`public_enterprise_threat_gate`, `public_cluster_provider_boundary_cleanup_gate`.

## DOC_PARSER_LISTENER_ENGINE_BOUNDARY

The listener binds the client socket to DBBT/LPREFACE state before parser
handoff. Parser workers can translate and return evidence, but they cannot
assert authentication, authorization, transaction finality, or recovery state.
The engine accepts only versioned public ABI calls or admitted SBLR envelopes.

SBSQL parser contracts must stay SBSQL-native. Reference syntax is mapped to
ScratchBird operation families through parser work, not pasted into SBSQL as a
foreign dialect.

Public proof anchors: `engine_listener_dbbt_lpreface_binding_conformance`,
`engine_listener_parser_contract_freeze_gate`, `engine_listener_sbsql_parser_sync_gate`.

## DOC_STARTUP_OPEN_LIFECYCLE

Startup validates configuration, service identity, runtime directories, secure
defaults, listener lifecycle files, and platform support status before
admission. Database open validates headers, durable metadata versions, catalog
identity, transaction inventory, filespace headers, page checksums, security
catalogs, and recovery-required state before exposing writes.

Shutdown drains listener admission, stops management routes, closes parser
workers, fences unsafe recovery states, and preserves support evidence without
leaking protected material.

Public proof anchors: `public_default_config_check`,
`public_secure_defaults_gate`, `database_lifecycle_shutdown_conformance`.

## DOC_FILE_FORMAT_OVERVIEW

All durable metadata has explicit version and integrity handling. The public
format surfaces include database header, filespace header, page header, page
body layout, row data body, transaction inventory, catalog record formats,
index metadata, TOAST/overflow pages, datatype descriptors, security catalog
records, optimizer plan-cache records, and release version metadata.

Unsupported old formats, unknown future formats, tampered records, ambiguous
identity, and downgrade requests must fail closed.

Public proof anchors: `engine_listener_compatibility_upgrade_downgrade_gate`,
`public_release_version_metadata_gate`, `public_upgrade_migration_gate`.

## DOC_TRANSACTION_MGA_MODEL

Transactions use MGA/COW versioning. A committed transaction is visible to
newer valid snapshots. Rolled-back versions are never visible. Cleanup and
reclamation use authoritative cleanup horizons and never reclaim TOAST or row
versions reachable from a visible row.

Page finality is not transaction finality. Backup, archive, repair, runbook,
and support evidence never replace MGA transaction inventory.

Public proof anchors: `public_transaction_mga_cow_gate`,
`transaction_inventory_publish_fault_conformance`,
`engine_listener_mga_integrated_physical_cleanup_conformance`.

## DOC_ISOLATION_SEMANTICS

The release surface documents snapshot visibility and serializable-admission
proofs. Long-running readers, concurrent writers, cleanup, and index
maintenance must preserve visibility rules. Any uncertain recovery or
visibility state fails closed instead of silently admitting writes.

uncertain recovery or visibility state fails closed before write admission.

Public proof anchors: `engine_listener_serializable_isolation_conformance`,
`engine_listener_crash_recovery_certification_gate`,
`engine_listener_fuzz_property_invariant_suite_gate`.

## DOC_CRASH_RECOVERY_MODEL

Recovery is certified at critical transitions: before and after header writes,
after page writes, before metadata updates, during transaction publish, during
rollback/savepoint undo, during TOAST allocation/cleanup, during index merge,
during filespace growth, during agent replay, and during listener handoff,
drain, and stop.

Each transition must produce old state, new state, recovery-required state,
write-admission fencing, operator-required state, or corruption fail-closed
detection. Silent inconsistency is not an accepted outcome.

Public proof anchors: `engine_listener_crash_fault_campaign_gate`,
`engine_listener_crash_recovery_certification_gate`,
`public_crash_fault_gate`.

## DOC_SECURITY_MODEL

Security decisions are materialized from durable security state.
Explicit deny overrides allow. Authentication provider evidence, protected material handling,
management authorization, DBBT/LPREFACE handoff, TLS/channel binding, support
bundle redaction, and parser-worker quarantine are separate proof surfaces.

Public proof anchors: `engine_listener_materialized_authorization_conformance`,
`engine_listener_tls_channel_binding_conformance`,
`engine_listener_support_bundle_redaction_gate`.

## DOC_AUTH_PROVIDER_MODEL

Auth providers are admitted by configuration and provider contract. Unsupported
providers, missing protected material, downgrade attempts, malformed identity,
and replay attempts must fail closed with stable diagnostics. Provider evidence
does not become engine authority until materialized by engine security code.

Provider evidence does not become engine authority until materialized by engine security code.

Public proof anchors: `public_security_provider_contract_protected_material_gate`,
`public_authorization_durable_flow_gate`, `public_enterprise_threat_gate`.

## DOC_AUTHORIZATION_MODEL

Authorization is evaluated from durable principals, grants, denials, policy
packs, and materialized session context. Management routes and engine routes
must both prove authenticated authorization context. Parser routes cannot
promote translation evidence into authorization.

Parser routes cannot promote translation evidence into authorization.

Public proof anchors: `dpc_security_privilege_gate`,
`database_lifecycle_config_policy_security_provider_conformance`,
`engine_listener_management_envelope_conformance`.

## DOC_PAGE_INDEX_TOAST_MODEL

Row-page storage, page-body registration, allocation maps, index maintenance,
secondary-index cleanup, and TOAST/overflow storage are engine-owned durable
surfaces. Index candidates can accelerate lookup but never replace row-version
authority. TOAST cleanup must preserve values reachable from visible rows.

Public proof anchors: `public_page_body_checksum_agreement_gate`,
`engine_listener_index_family_dml_route_conformance`,
`public_toast_overflow_binary_descriptor_gate`.

## DOC_OPTIMIZER_MODEL

Optimizer plans are evidence used to select physical execution paths. Plan
cache metadata is versioned and invalidated on incompatible changes. Statistics
and hints may influence plan selection but never override authorization,
transaction visibility, row authority, or recovery fencing.

Public proof anchors: `engine_listener_optimizer_integrated_route_conformance`,
`public_optimizer_catalog_backed_planning_gate`,
`optimizer_enterprise_plan_cache_gate`.

## DOC_MEMORY_GOVERNANCE_MODEL

Memory governance uses explicit default manager configuration, query
reservations, spill policy, pressure response, high-water evidence, secure temp
workspace handling, and support-bundle summaries.
Memory-pressure diagnostics are evidence-only and cannot change durable transaction finality.

Public proof anchors: `engine_listener_memory_integrated_conformance`,
`public_query_memory_reservation_gate`, `public_memory_pressure_executor_gate`.

## DOC_CONFIGURATION_REFERENCE

Configuration docs cover release profile, secure defaults, listener
management, parser-worker pool, service identity, memory policy, policy packs,
cluster provider admission, runtime directories, support-bundle settings, and
debug or diagnostic toggles.
Single-node manager MCP file/env secrets require explicit
`manager.auth.mcp_secret_rights` in enterprise profile, and wildcard secret
rights are developer-profile only.
Manager support-bundle `scope` and `redaction_profile` command arguments are
bounded tokens; malformed, path-shaped, or non-allowlisted values fail closed
before any bundle artifact is created.
Manager config validation and reload commands may only reference the manager's
already configured config file; arbitrary, relative, or alternate `config_ref`
paths fail closed and accepted references are redacted in responses.
Manager control command payloads are command-specific allowlisted envelopes:
unknown fields, duplicate fields, and invalid argument values fail closed before
the command reaches listener, config, support-bundle, or status handling.
Mutating manager operations require bounded idempotency keys with stable token
syntax; malformed replay keys fail closed before side effects or bundle
artifact creation.

Unsupported or unsafe configurations fail closed. Cluster-positive routes are provider-required
and compile-time gated.

Public proof anchors: `engine_listener_release_profile_completeness_gate`,
`public_default_config_check`, `public_secure_defaults_gate`.

## DOC_OPERATIONAL_RUNBOOK

Operator runbooks are maintained in `project/docs/admin/PUBLIC_ADMIN_RUNBOOKS.md`.
They cover create, open, config defaults, security policy, backup, restore,
verify, repair, memory pressure, sweep, archive, diagnostics, unsupported
features, and upgrade.

Runbook output is evidence-only and never replaces engine authority.

Public proof anchors: `public_admin_runbook_gate`,
`public_runbook_consistency_check.py`, `public_diagnostic_stability_gate`.

## DOC_TROUBLESHOOTING_GUIDE

Troubleshooting starts with stable diagnostics, support-bundle redaction,
management route status, lifecycle files, filespace summaries, transaction
inventory summaries, security context summaries, index readiness summaries,
optimizer readiness summaries, and recovery-required diagnostics.

Public proof anchors: `engine_listener_operational_readiness_gate`,
`database_lifecycle_observability_conformance`,
`dpc_management_observability_support_bundle_gate`.

## DOC_SUPPORT_BUNDLE_GUIDE

Support bundles are redacted by default. They can include diagnostics,
configuration summaries, transaction inventory summaries, filespace health,
security context summaries, index readiness, optimizer readiness, memory
high-water evidence, and listener/parser lifecycle state. They must not leak
secrets or protected material.

Support bundles must not leak secrets or protected material.

Public proof anchors: `engine_listener_support_bundle_redaction_gate`,
`engine_listener_support_bundle_conformance`,
`public_crypto_entropy_policy_gate`.

## DOC_UPGRADE_GUIDE

Upgrade documentation covers current-version accept, supported migration,
unsupported old-format refusal, future-format refusal, downgrade refusal,
parser contract versioning, plan-cache invalidation, policy-pack compatibility,
and operator diagnostics.

Public proof anchors: `engine_listener_compatibility_upgrade_downgrade_gate`,
`public_upgrade_migration_gate`,
`database_lifecycle_upgrade_migration_conformance`.

## DOC_BACKUP_RESTORE_INTERACTION_GUIDE

Backup, restore, archive, repair, and sweep documentation explains interaction
with MGA horizons and transaction inventory. Backup records and archive
records are not transaction finality authority. Restore and repair evidence
must preserve identity handling and fail closed on tamper or unsupported state.

Backup records and archive records are not transaction finality authority.

Public proof anchors: `public_backup_forward_session_gate`,
`public_backup_update_coverage_gate`, `public_archive_before_reclaim_gate`.

## DOC_LIMITATIONS_SUPPORT_STATES

Known limitations are explicit:

- Linux is the first fully proven platform lane in this repository state.
- Windows x64 and FreeBSD are target platform lanes pending runner proof.
- macOS is out of scope for the first public release.
- Cluster-positive behavior is provider-required and cluster-provider-only in
  public core builds.
- Unsupported features must fail closed with stable diagnostics.
- Parser and driver behavior cannot assert engine authority.
- Documentation and runbooks are evidence-only.

Support states are documented in
`project/docs/release/PUBLIC_SUPPORT_RELEASE_LIFECYCLE.md`.

Public proof anchors: `public_platform_matrix_gate`,
`public_unsupported_feature_gate`, `public_api_abi_compat_gate`.
