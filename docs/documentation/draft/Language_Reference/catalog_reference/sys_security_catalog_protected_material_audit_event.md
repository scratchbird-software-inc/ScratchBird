# sys.security.catalog.protected_material_audit_event Catalog Reference

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `catalog_sys_security_catalog_protected_material_audit_event`


## Role

`sys.security.catalog.protected_material_audit_event` is a system catalog surface. It records durable metadata used by the binder, engine verifier, optimizer, security layer, support diagnostics, bridge rendering, or transaction model.

Catalog rows are not parser authority. They are visible through authorized catalog projections, SHOW/DESCRIBE surfaces, information-style views, or support tooling. Base catalog mutation must go through engine-managed catalog operations.

## Keys And Columns

| Column | Type Family | Requirement |
| --- | --- | --- |
| audit_event_uuid | UUID/text | Stable audit event identity. |
| protected_material_uuid | UUID | Material involved. |
| protected_material_version_uuid | nullable UUID | Version involved. |
| actor_uuid | UUID | Actor identity, redacted in projections where policy requires. |
| event_kind | enum | create, add_version, resolve, release, deny, purge, policy_change, inspect. |
| decision | enum | allow or deny. |
| diagnostic_code | nullable text | Diagnostic emitted for denial or refusal. |
| redacted_detail | text | Redacted event detail. |
| event_epoch_millis | uint64 | Event time from engine audit context. |
| local_transaction_id | uint64 | MGA transaction ID associated with event. |
| catalog_generation_id | uint64 | Catalog generation associated with event. |
| redaction_applied | boolean | Must be true for protected material events. |
| SCT-GATE-001 | Required tables/views exist or are explicitly deferred. | Automated/report |
| SCT-GATE-002 | Required primary keys and foreign-key relationships exist. | Automated |
| SCT-GATE-003 | Required epoch/version/status columns exist. | Automated |
| SCT-GATE-004 | Visibility and redaction policies apply to all catalog reads. | Security test |
| object_uuid | UUID | Stable row identity, non-null, immutable after creation. |
| object_kind | enum/text | Registered catalog object kind. |
| schema_path | text/domain | Must be valid under local `sys.*` or cluster `cluster.sys.*` placement authority. |
| definition_version | uint64 | Monotonic row definition version. |
| schema_epoch | uint64 | Schema epoch at row visibility. |
| security_epoch | uint64 | Security policy epoch for visibility and admission. |
| resource_epoch | uint64 | Resource dependency epoch or zero when no resource dependency exists. |
| status | enum | active, deferred, disabled_by_policy, bridge_only, render_only, retired, unsupported_by_policy, or quarantined. |
| owner_uuid | UUID | Owning schema, package, database, local system authority, or cluster authority when cluster table exists. |
| created_txn_uuid | UUID/system-event UUID | Creating transaction or system event. |
| retired_txn_uuid | nullable UUID/system-event UUID | Retiring transaction or system event when retired/quarantined. |
| definition_hash | binary digest | Digest of behavior-affecting row fields. |
| visibility_class | enum | Public, authenticated, administrator, SysArch, support_redacted, security_redacted, or internal_only. |
| audit_ref | nullable UUID/text | Audit or support event reference. |

## Full Definition Extract

### Catalog Table `sys.security.catalog.protected_material_audit_event`

Primary key: `audit_event_uuid`

Required columns:

| Column | Type family | Requirement |
| --- | --- | --- |
| `audit_event_uuid` | UUID/text | Stable audit event identity. |
| `protected_material_uuid` | UUID | Material involved. |
| `protected_material_version_uuid` | nullable UUID | Version involved. |
| `actor_uuid` | UUID | Actor identity, redacted in projections where policy requires. |
| `event_kind` | enum | create, add_version, resolve, release, deny, purge, policy_change, inspect. |
| `decision` | enum | allow or deny. |
| `diagnostic_code` | nullable text | Diagnostic emitted for denial or refusal. |
| `redacted_detail` | text | Redacted event detail. |
| `event_epoch_millis` | uint64 | Event time from engine audit context. |
| `local_transaction_id` | uint64 | MGA transaction ID associated with event. |
| `catalog_generation_id` | uint64 | Catalog generation associated with event. |
| `redaction_applied` | boolean | Must be true for protected material events. |

Audit event rows are retained when protected reference reachability is purged. Support bundles and diagnostics may include these rows only through redacted evidence views.

## Mandatory foreign-key relationships

1. `sys.domain_descriptor.base_descriptor_uuid` references
   `sys.type_descriptor.descriptor_uuid`.
2. `sys.domain_element.domain_uuid` references
   `sys.domain_descriptor.domain_uuid`.
3. `sys.donor_type_mapping.descriptor_uuid` references
   `sys.type_descriptor.descriptor_uuid` when non-null.
4. `sys.donor_type_mapping.domain_uuid` references
   `sys.domain_descriptor.domain_uuid` when non-null.
5. `sys.type_capability.descriptor_uuid` references
   `sys.type_descriptor.descriptor_uuid`.
6. Operation, cast, aggregate, and window signatures reference type/domain
   descriptors by UUID.
7. Index/statistics/metadata/backup/replication/transport profiles reference
   the descriptor/domain they control.
8. Resource dependencies reference both dependent object and resource UUID.


## Conformance gates

| Gate | Requirement | Evidence |
| --- | --- | --- |
| `SCT-GATE-001` | Required tables/views exist or are explicitly deferred. | Automated/report |
| `SCT-GATE-002` | Required primary keys and foreign-key relationships exist. | Automated |
| `SCT-GATE-003` | Required epoch/version/status columns exist. | Automated |
| `SCT-GATE-004` | Visibility and redaction policies apply to all catalog reads. | Security test |


## Direct Amendment: Catalog Support for Compatibility Domains and Method Translation


The system catalog SHALL include catalog structures sufficient to define compatibility domains and donor method translation without depending on donor runtimes. The following logical catalog families are required; physical table names may follow the catalog naming rules for the sys schema.

| Catalog family | Required purpose |
| --- | --- |
| `TYPE_COMPATIBILITY_PROFILE` | Records donor family, donor version profile, compatibility mode, parser family, render rules, feature gates, and profile status |
| `DOMAIN_INTERFACE` | Records object-like, media-like, URI-like, structured, distinct, and opaque domain interfaces |
| `DOMAIN_ATTRIBUTE` | Records exposed attributes, element paths, types, ordinals, visibility, nullability, and donor names |
| `DOMAIN_METHOD` | Records method descriptors, constructor descriptors, package routine mappings, argument descriptors, return descriptors, and visibility |
| `DOMAIN_LOCATOR_POLICY` | Records locator target class, dereference policy, privilege policy, backup/restore policy, replication policy, and external access policy |
| `DOMAIN_OPAQUE_PAYLOAD_POLICY` | Records serialization identity, validation behavior, comparison behavior, hash behavior, display behavior, and preservation policy |
| `DONOR_METHOD_BINDING` | Records donor-to-ScratchBird method translation, SBLR lowering, C++ UDR bridge target, inverse rendering, and diagnostic mapping |
| `COMPATIBILITY_CONFORMANCE_REF` | Records conformance manifest references for datatype, method, metadata, and diagnostic behavior |

Catalog entries that reference executable behavior SHALL reference ScratchBird-native routines, generated SBLR, or registered C++ UDR packages only. The catalog SHALL NOT contain trusted execution targets for non-C++ donor runtimes.


## Direct Amendment: MGA Transaction Catalog Tables


The system catalog SHALL include logical catalog tables for the ScratchBird MGA transaction model. The authoritative table definitions are in `mga-transactions/appendix-mga-durable-layouts-and-catalog.md`.

Required catalog tables:

| Table | Required purpose |
| --- | --- |
| `sys.catalog.mga_checkpoint` | Checkpoint identity, root page, root-set hash, state, and transaction boundaries. |
| `sys.catalog.mga_transaction_inventory` | Transaction state, durability state, lineage, commit order, and archive linkage. |
| `sys.catalog.mga_horizon` | Named transaction horizons, owners, blockers, and advancement state. |
| `sys.catalog.mga_retention_pin` | Policy-owned retention ranges that block cleanup, reclaim, truncation, and deletion. |
| `sys.catalog.mga_archive_manifest` | Archive generation, lineage, manifest root, retention mode, and archive state. |
| `sys.catalog.mga_archive_segment` | Archive segment transaction range, hash, key lineage, retention pin, and physical locator. |
| `sys.catalog.mga_backup_coverage` | Backup-forward coverage chain summary and gap state. |
| `sys.catalog.mga_backup_coverage_segment` | Required stream/archive segments for backup-forward replay. |
| `sys.catalog.mga_stream_retention` | Side-stream class, authority class, consumer progress, truncation proof, and gap state. |
| `sys.cluster_transaction` | Cluster transaction coordinator state, epoch, order, idempotency, quorum, and trusted read/write-set hashes. |
| `sys.cluster_transaction_participant` | Participant state, visibility gate, provisional root, and decision linkage. |
| `sys.catalog.mga_maintenance_operation` | Restartable maintenance operation progress, state, policy, target, and errors. |

Silent omission of these catalog contracts is specification drift. Any deferred implementation must create an explicit deferred-decision record and a conformance gate waiver.

## Amendment: MGA Catalog Table Definition Requirements

MGA catalog table definitions must include or explicitly reference the table definitions required by `mga-transactions/appendix-mga-catalog-ddl-schema-and-object-model.md`, `mga-transactions/appendix-mga-management-operation-catalog.md`, and `mga-transactions/appendix-mga-metrics-catalog-and-telemetry-binding.md`.

Every MGA catalog table must define UUID identity, generation behavior, authority scope, ownership, primary key, required secondary indexes, retention rule, security/redaction class when sensitive, evidence linkage when mutating, and conformance search key.

Cluster catalog tables must include standalone absence behavior and must not be created in standalone databases.


## Direct amendment: zero-grey system catalog table definition contract


This appendix controls the logical table-definition layer for local `sys.*` catalog objects. A third-party implementor must be able to create catalog tables, derived views, constraints, dependency edges, cache invalidation records, diagnostics, and conformance tests from this appendix and its cited private specification appendices without inspecting source code or importing donor catalog behavior.

### Table definition completeness rule

Every catalog table or derived view listed by this appendix must have an implementation-grade definition that includes:

| Definition item | Required content |
| --- | --- |
| `schema_path` | Exact local `sys.*` path, or exact `cluster.sys.*` path only when cluster catalog authority exists. |
| `table_name` | Stable table or view name. |
| `table_uuid` | Stable table identity UUID assigned by catalog bootstrap. |
| `authority_spec` | Private docs path and search key controlling the table. |
| `table_class` | Base table, derived view, compatibility view, metric surface, evidence surface, or deferred definition. |
| `owner_authority` | Catalog, engine, security, resource, parser, UDR, MGA, cluster, or metrics authority that owns truth. |
| `lifecycle` | Proposed, active, disabled_by_policy, bridge_only, render_only, deferred, unsupported_by_policy, retired, or quarantined. |
| `primary_key` | Exact primary key columns and uniqueness rule. |
| `unique_keys` | Exact alternate uniqueness constraints, if any. |
| `foreign_keys` | Exact referenced table, referenced key, dependency strength, and invalidation rule. |
| `required_indexes` | Required physical or logical indexes needed for correctness or performance. |
| `column_definitions` | Complete field table for every column using the master field-definition format. |
| `visibility_rule` | Policy snapshot and redaction rule for reads. |
| `mutation_rule` | Allowed insert/update/delete/retire/quarantine behavior. |
| `transaction_rule` | Transactional DDL/DML behavior and rollback behavior. |
| `cache_invalidation_rule` | Plan, parser, driver, UDR, index, statistics, backup, replication, transport, diagnostic, or support-bundle invalidation. |
| `diagnostics` | Specific diagnostic vectors for every invalid state. |
| `metrics` | `sys.metrics.catalog.*` metrics emitted for mutation, visibility, invalidation, and validation. |
| `conformance_keys` | Positive and negative conformance gates. |

A table definition that only lists a table name and purpose is not implementation-ready. A table definition that lists a column without type, range, default, authority, persistence, visibility, redaction, invalid-state behavior, and conformance key is not implementation-ready.

### Required common columns for all base catalog tables

Every base catalog table controlled by this appendix must include the common columns from `SCS-COMMON-CATALOG-COLUMNS` and the zero-grey system catalog schema amendment. At minimum the base row contract is:

| Column | Physical/logical type | Required behavior |
| --- | --- | --- |
| `object_uuid` | UUID | Stable row identity, non-null, immutable after creation. |
| `object_kind` | enum/text | Registered catalog object kind. |
| `schema_path` | text/domain | Must be valid under local `sys.*` or cluster `cluster.sys.*` placement authority. |
| `definition_version` | uint64 | Monotonic row definition version. |
| `schema_epoch` | uint64 | Schema epoch at row visibility. |
| `security_epoch` | uint64 | Security policy epoch for visibility and admission. |
| `resource_epoch` | uint64 | Resource dependency epoch or zero when no resource dependency exists. |
| `status` | enum | active, deferred, disabled_by_policy, bridge_only, render_only, retired, unsupported_by_policy, or quarantined. |
| `owner_uuid` | UUID | Owning schema, package, database, local system authority, or cluster authority when cluster table exists. |
| `created_txn_uuid` | UUID/system-event UUID | Creating transaction or system event. |
| `retired_txn_uuid` | nullable UUID/system-event UUID | Retiring transaction or system event when retired/quarantined. |
| `definition_hash` | binary digest | Digest of behavior-affecting row fields. |
| `visibility_class` | enum | Public, authenticated, administrator, SysArch, support_redacted, security_redacted, or internal_only. |
| `audit_ref` | nullable UUID/text | Audit or support event reference. |

Derived views may omit stored common columns only if the view definition maps each visible row back to authoritative base-row UUIDs and preserves visibility/redaction policy.

### Column definition format

Every column definition must specify:

- column name;
- physical type;
- logical type;
- nullability;
- valid range or enum values;
- default value or `no_default`;
- authority source;
- persistence and serialization behavior;
- versioning rule;
- visibility class;
- redaction rule;
- whether the column participates in primary key, unique key, foreign key, dependency edge, cache key, or metric dimension;
- invalid-state diagnostic;
- invalid-state behavior;
- conformance key.

A nullable foreign key must define whether null means not applicable, deferred, render-only, unsupported, hidden by policy, or unresolved. Silent ambiguous nulls are forbidden.

### Local and cluster table placement

Tables whose names begin with `sys.cluster_` in older text must be classified before implementation:

| Classification | Placement |
| --- | --- |
| Local observation of cluster behavior | `sys.*` with local-only authority and no cluster-wide finality claim. |
| Cluster-wide authority | `cluster.sys.*` and absent when no cluster exists. |
| Cluster-capable type transport profile | local `sys.*` profile row unless it records cluster membership, route ownership, or cluster transaction authority. |
| Deferred cluster table | no physical table until cluster specification admits it. |

`sys.catalog.cluster_type_transport_profile` remains a local type transport compatibility profile unless a cluster authority appendix explicitly creates a `cluster.sys.*` transport table.

`sys.cluster_transaction` and `sys.cluster_transaction_participant` are cluster-authority tables. They must be placed under `cluster.sys.*` when a cluster exists and must be absent in standalone databases. A standalone database may expose local read-only compatibility views that report cluster support absent, but those views must not fabricate cluster rows.

### Table creation algorithm

```text
procedure create_system_catalog_table(definition):
    validate definition path and search key are private docs references
    validate schema_path placement against local or cluster authority
    validate table_uuid and table_name uniqueness
    validate every common column and table-specific column definition
    validate primary key, unique keys, foreign keys, and required indexes
    validate visibility, mutation, transaction, and cache invalidation rules
    validate diagnostics and metrics exist
    create table or derived view inside catalog bootstrap transaction
    record table definition hash and catalog generation
    publish table only after transaction commit and cache invalidation registration
```

If validation fails, no partial table definition may become visible. Bootstrap failures before any database exists must be rollback-safe under the bootstrap specification.

### Mutation and derived view rules

Base catalog tables are transactionally mutable only through engine-managed catalog operations. Parser SQL text, donor metadata requests, driver metadata requests, support-bundle export, and diagnostics rendering must not mutate base catalog tables directly.

Derived views must define:

- source base tables;
- join/key rules;
- redaction behavior;
- hidden-row behavior;
- donor rendering profile when applicable;
- stale behavior when source epochs change;
- diagnostic emitted when rendering cannot preserve semantics;
- whether trusted UUIDs are exposed.

A donor compatibility view is a rendering surface. It does not become engine identity, transaction authority, recovery authority, or execution authority.

### Required diagnostics

The catalog table-definition layer must provide diagnostic vectors for:

- `CATALOG.TABLE_DEFINITION_INCOMPLETE`;
- `CATALOG.TABLE_UUID_DUPLICATE`;
- `CATALOG.TABLE_NAME_DUPLICATE`;
- `CATALOG.TABLE_SCHEMA_PATH_INVALID`;
- `CATALOG.COLUMN_DEFINITION_INCOMPLETE`;
- `CATALOG.COLUMN_NULL_SEMANTICS_AMBIGUOUS`;
- `CATALOG.PRIMARY_KEY_MISSING`;
- `CATALOG.UNIQUE_KEY_INVALID`;
- `CATALOG.FOREIGN_KEY_INVALID`;
- `CATALOG.REQUIRED_INDEX_MISSING`;
- `CATALOG.DERIVED_VIEW_AUTHORITY_MISSING`;
- `CATALOG.DERIVED_VIEW_REDACTION_INVALID`;
- `CATALOG.CLUSTER_TABLE_IN_STANDALONE_FORBIDDEN`;
- `CATALOG.TABLE_BOOTSTRAP_PARTIAL_VISIBILITY_FORBIDDEN`.

Each diagnostic must include table name, table UUID when assigned, schema path, authority search key, affected column when applicable, and redaction state.

### Metrics

Catalog table-definition metrics must be exposed under `sys.metrics.catalog.table_definition.*` locally. Cluster aggregate catalog table-definition metrics must use `cluster.sys.metrics.catalog.table_definition.*` only when cluster governance exists.

Required local metrics are:

- `sys.metrics.catalog.table_definition.total_count`;
- `sys.metrics.catalog.table_definition.deferred_count`;
- `sys.metrics.catalog.table_definition.base_table_count`;
- `sys.metrics.catalog.table_definition.derived_view_count`;
- `sys.metrics.catalog.table_definition.invalid_definition_count`;
- `sys.metrics.catalog.table_definition.schema_path_rejection_count`;
- `sys.metrics.catalog.table_definition.column_incomplete_count`;
- `sys.metrics.catalog.table_definition.foreign_key_invalid_count`;
- `sys.metrics.catalog.table_definition.required_index_missing_count`;
- `sys.metrics.catalog.table_definition.cluster_absent_rejection_count`.

### Conformance requirements

Catalog table-definition conformance must prove:

- every listed table has a complete table definition record;
- every base table includes required common columns;
- every column has complete field-definition metadata;
- every primary key, unique key, foreign key, and required index is defined;
- local `sys.*` and cluster `cluster.sys.*` placement rules are enforced;
- cluster-authority tables are absent when no cluster exists;
- derived views preserve base-row UUID authority and redaction policy;
- donor views cannot become engine authority;
- invalid table definitions fail atomically without partial visibility;
- parser, driver, and support-bundle surfaces cannot mutate base catalog tables directly;
- cache invalidation reaches every declared dependent surface.

### Completion rule

A system catalog table is implementation-ready only when it has a complete table-definition record, complete column definitions, primary key, foreign keys, required indexes, lifecycle states, mutation rules, derived-view rules if applicable, local/cluster placement, diagnostics, metrics, conformance tests, and implementation trace rows.

## Operational Boundaries

- Base rows require UUID identity and lifecycle metadata.
- Visibility is policy controlled and may use redaction.
- Derived views must preserve base-row authority and must not become engine identity.
- Donor compatibility projections are rendering surfaces only.

## Example Inspection

```sql
select *
from sys.security.catalog.protected_material_audit_event
limit 20;
```
