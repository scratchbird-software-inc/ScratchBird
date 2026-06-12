# Public Release Foundation Closure Execution_Plan

Status: pending implementation
Created: 2026-05-10
Owner: ScratchBird public release foundation coordinator
Search key: `PUBLIC-RELEASE-FOUNDATION-CLOSURE`

## Purpose

Close the first public zero-grey target set from the implementation inventory.
This execution_plan does not attempt to close every public gap in one pass. It closes
the foundation that later parser, reference, driver, cloud, and release work must
stand on:

```text
zero-grey release gate authority
  -> SBWP/TLS transport security
  -> authoritative catalog tables and user-facing catalog projections
  -> logical constraints and keys
  -> MGA recovery, checkpoint, backup, delta, and PITR proof
```

The controlling input is the machine-readable registry at
`public_audit_summary`, generated from
`public_audit_summary`.

## Target Scope

This execution_plan closes five target groups and their direct dependency gaps.

| Target | Registry gaps | Purpose |
| --- | --- | --- |
| Zero-grey release authority | `SB-PUBLIC-GAP-0143` | Make release closure machine-enforced through conformance manifests and release gate records. |
| Transport security | `SB-PUBLIC-GAP-0066` | Land SBWP/TLS and listener/server wire protection instead of config-only drift. |
| Catalog closure | `SB-PUBLIC-GAP-0009` through `SB-PUBLIC-GAP-0018` | Materialize low-level catalog authority, readable catalog views, `sys.information`, legacy SQL synonyms, dependency graph, diagnostics, metadata, generation, names, and comments. |
| Constraints and keys | `SB-PUBLIC-GAP-0026` | Implement logical integrity descriptors, DDL, enforcement, diagnostics, and transaction behavior. |
| MGA recovery/backup/PITR | `SB-PUBLIC-GAP-0002`, `SB-PUBLIC-GAP-0025`, `SB-PUBLIC-GAP-0027`, `SB-PUBLIC-GAP-0028`, `SB-PUBLIC-GAP-0029`, `SB-PUBLIC-GAP-0030`, `SB-PUBLIC-GAP-0035` | Close checkpoint, dirty manifest, backup/restore proof, delta coverage, PITR, and page durability ordering. |

The target list is also recorded in
`artifacts/PUBLIC_RELEASE_FOUNDATION_TARGET_GAPS.csv`.

## Non-Negotiable Rules

- No placeholder, stub, or deferral may be used to mark a target gap closed.
- The engine remains SBLR-only. SQL text remains parser input only.
- The engine remains MGA-based. No WAL, redo-log, or reference transaction model may
  become recovery authority.
- Catalog tables hold UUID authority. Human-readable names live only in resolver
  and projection layers.
- `sys.catalog` is low-level authority; `sys.catalog_readable` and
  `sys.information` are client-facing projections.
- `sys.information_schema` is a first-class SQL synonym object under `sys` that
  points to `sys.information`; it has its own synonym UUID but must resolve to
  the same final schema UUID and must not become a child parent.
- SQL object synonyms are not extra name rows. They are catalog objects with
  target object references, bounded chain resolution, cycle detection, language
  conflict checks, dependency edges, and MGA rollback behavior.
- TLS/auth decisions must be engine-authorized; listener/parser/driver layers
  may transport credentials and proof but must not become the security authority.
- Release gates must be evidence-based. A report may not close a gap unless the
  matching executable gate passes.
- Cluster/private work remains out of scope except for fail-closed boundaries
  needed by public single-node behavior.

## Implementation Order

```text
P0 registry and release-gate authority
  -> P0H hardening inputs and write-scope controls
  -> P0S canonical SQL object synonym contract authority
  -> P1 SBWP/TLS transport security
  -> P2 SQL object synonym implementation and catalog authority/projections
  -> P3 logical integrity
  -> P4 MGA recovery, backup, delta, and PITR
  -> P5 end-to-end integration and target zero-grey closure
```

## Pre-Implementation Hardening Artifacts

Implementation must not start until these artifacts exist and are reviewed:

- `artifacts/TARGET_EVIDENCE_MANIFEST.csv`
- `artifacts/PREFLIGHT_BASELINE_INVENTORY.csv`
- `artifacts/PERSISTENT_FORMAT_MIGRATION_POLICY.md`
- `artifacts/FAULT_INJECTION_MATRIX.csv`
- `artifacts/CATALOG_PHYSICAL_INDEX_PROFILE.md`
- `artifacts/INFORMATION_PROJECTION_NAMING_DECISION.md`
- `artifacts/SYNONYM_OBJECT_SEMANTICS.md`
- `artifacts/TLS_FIXTURE_POLICY.md`
- `artifacts/CONSTRAINT_INDEX_DEPENDENCY_POLICY.md`
- `artifacts/FULL_ROUTE_ACCEPTANCE_FIXTURE.md`
- `artifacts/AGENT_WRITE_SCOPE_MATRIX.csv`
- `artifacts/AGENT_STATUS.csv`

These artifacts are part of the execution_plan, not optional commentary. They prevent
false closure by defining evidence, durable-format migration, failure injection,
TLS fixture behavior, catalog physical index profiles, user-facing projection
names, SQL object synonym semantics, constraint backing-index dependencies,
full-route acceptance, agent write ownership, and the heartbeat status schema
before implementation starts.

The product behavior authority for SQL object synonyms is the manifest-listed
canonical contract
`public_contract_snapshot`.
The execution_plan artifact records execution evidence only and must not supersede the
canonical spec.

## Pre-P4 Recovery Proof Artifact

`artifacts/MGA_RECOVERY_PROOF_MODEL.md` is the blocking proof model for P4. It
must be reviewed and recorded as `PRF-040` evidence before any checkpoint,
dirty-manifest, backup, delta, PITR, or page durability implementation slice
starts. The model keeps recovery authority on MGA transaction inventory, page
generation state, checkpoint records, dirty manifests, operation envelopes, and
coverage proof. It forbids WAL, redo-log, reference transaction authority, or parser
transaction authority as recovery truth.

## Required CTest Labels

Exact target names are materialized during implementation, but these labels are
mandatory:

```text
public_release_foundation
public_release_foundation_target_gap_registry
public_release_foundation_target_evidence_manifest_gate
public_release_foundation_zero_grey_target_gate
public_release_foundation_target_zero_grey_gate
global_public_zero_grey_non_target_regression_audit
release_gate_record_conformance
conformance_manifest_registry_gate
persistent_format_migration_policy_gate
fault_injection_matrix_gate
agent_write_scope_gate
sql_object_synonym_spec_authority_gate
sbwp_tls_server_listener_gate
sbwp_tls_engine_auth_gate
sbwp_tls_negative_policy_gate
tls_fixture_policy_gate
catalog_common_descriptor_gate
sys_catalog_table_materialization_gate
catalog_name_comment_resolver_gate
catalog_synonym_object_gate
catalog_synonym_resolution_gate
catalog_synonym_parent_remap_gate
catalog_synonym_depth_cycle_gate
catalog_generation_mga_gate
catalog_dependency_api_gate
catalog_physical_index_profile_gate
information_projection_naming_gate
catalog_sql_object_synonym_semantics_gate
sys_catalog_readable_projection_gate
sys_information_projection_gate
sys_information_schema_synonym_gate
catalog_diagnostic_vector_gate
catalog_wire_metadata_gate
constraint_catalog_descriptor_gate
constraint_ddl_gate
constraint_dml_enforcement_gate
constraint_transaction_visibility_gate
constraint_diagnostic_gate
constraint_index_dependency_gate
mga_recovery_proof_model_gate
mga_checkpoint_generation_gate
mga_dirty_manifest_emission_gate
mga_recovery_idempotency_gate
backup_restore_manifest_proof_gate
mga_delta_coverage_gate
pitr_rollforward_gate
page_durability_ordering_gate
full_route_acceptance_fixture_gate
public_release_foundation_full_route_gate
public_release_foundation_final_audit
```

## Agent Execution Protocol

When implementation begins, the coordinator may split the work across agents by
write scope:

- Release gate agent: conformance manifest schema, target gate, registry status
  updater, and CTest labels.
- TLS agent: listener/server/SBWP TLS termination, mTLS/channel binding, and
  negative policy tests.
- Catalog agent: catalog row codecs, physical tables, resolvers, projections,
  dependency graph, diagnostics, and metadata tests.
- Constraint agent: constraint catalog descriptors, DDL, DML enforcement,
  transaction integration, and diagnostics.
- MGA recovery agent: checkpoint, dirty manifest, backup/restore, delta, PITR,
  durability ordering, and crash/restart tests.

Agents must write heartbeat/status updates to `artifacts/AGENT_STATUS.csv` at
least every five minutes during long-running implementation. The coordinator
must stop only for a real design block, failing validation, merge conflict, or
security/authority contradiction.

`artifacts/AGENT_STATUS.csv` is seeded before implementation starts. Agents must
preserve its columns and append or update rows using `timestamp_utc`, `agent`,
`phase`, `current_slice`, `status`, `blocked_by`, `last_update`, `next_action`,
and `evidence_refs`.

## Definition Of Done

This execution_plan is complete only when:

- Every row in `artifacts/PUBLIC_RELEASE_FOUNDATION_TARGET_GAPS.csv` is closed
  as `implemented_in_full` in the public gap registry.
- The target zero-grey gate passes with zero open target gaps.
- The global public zero-grey gate may still fail only for non-target public
  gaps; a non-target regression audit proves no target gap and no previously
  closed gap regressed.
- All mandatory CTest labels exist and pass.
- The target evidence manifest records passing gate evidence for every target
  gap before status is changed to `implemented_in_full`.
- Canonical synonym contracts are manifest-listed and pass the
  `sql_object_synonym_spec_authority_gate` before synonym implementation gates
  run.
- Persistent-format changes have format-version, upgrade, repair, and refusal
  behavior before durable structures are written.
- Fault-injection gates cover the required security, catalog, constraint, and
  recovery failure modes.
- Agent heartbeat evidence is present in `artifacts/AGENT_STATUS.csv` for every
  agent-managed long-running slice.
- `SB-PUBLIC-GAP-0066` is no longer drift: server/listener SBWP/TLS behavior is
  implemented, tested, and documented.
- `sys.catalog`, `sys.catalog_readable`, and `sys.information` have real
  descriptors, row generators/views, diagnostics, and security-filtered
  behavior.
- `sys.information_schema` exists only as a first-class synonym object that
  reconciles legacy client references to the same final schema UUID as
  `sys.information`; children created through the synonym path are parented to
  `sys.information`.
- Constraints and keys enforce correctness through catalog authority and MGA
  transaction visibility.
- MGA checkpoint/recovery, dirty manifest, backup/restore proof, delta coverage,
  PITR, and page durability ordering are implemented without WAL semantics.
- `artifacts/MGA_RECOVERY_PROOF_MODEL.md` is cited by the P4 evidence and the
  final audit for recovery-related target gaps.
- `SPEC_IMPLEMENTATION_AUDIT_MATRIX.csv`, `TRACKER.csv`,
  `ACCEPTANCE_GATES.csv`, and the public gap registry are updated with final
  evidence.
