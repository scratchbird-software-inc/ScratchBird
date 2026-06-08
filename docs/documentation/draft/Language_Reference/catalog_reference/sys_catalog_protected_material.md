# sys.catalog.protected_material Catalog Reference

This page documents the authorized catalog surface that records protected
material identity and lifecycle metadata. It does not expose raw protected
values.

Generation task: `catalog_sys_catalog_protected_material`

Related pages: [Binary, UUID, And Protected Values](../data_types/binary_uuid_and_protected_values.md),
[Security And Sandboxing](../core_paradigms/security_and_sandboxing.md),
[sys.catalog.protected_material_version](sys_catalog_protected_material_version.md),
[sys.catalog.protected_material_policy_binding](sys_catalog_protected_material_policy_binding.md), and
[sys.security.catalog.protected_material_audit_event](sys_security_catalog_protected_material_audit_event.md).

## Role

`sys.catalog.protected_material` is the durable identity record for secret,
credential, key, token, protected binary, protected text, protected diagnostic,
or other release-controlled material.

The table stores metadata and references. It must not store or render plaintext
secret material through ordinary catalog projections.

## Keys And Columns

Primary key: `protected_material_uuid`

| Column | Type Family | Requirement |
| --- | --- | --- |
| `protected_material_uuid` | UUID | Stable protected-material identity. |
| `object_class` | enum/text | Protected-material class or admitted subclass. |
| `owner_scope_uuid` | UUID | Owning database, schema, package, principal, security scope, or system scope. |
| `purpose_class` | enum/text | Default purpose for access, release, rotation, backup, replication, or support use. |
| `storage_class` | enum | `wrapped`, `split`, `external_reference`, `derived`, `redacted`, or other admitted class. Plaintext catalog storage is not an ordinary public class. |
| `active_version_uuid` | nullable UUID | Active version visible for the current catalog generation. |
| `lifecycle_state` | enum | `active`, `disabled_by_policy`, `retained_no_active_version`, `purged`, `quarantined`, or `archived`. |
| `retention_policy_uuid` | UUID | Retention policy for metadata and references. |
| `access_policy_uuid` | UUID | Metadata visibility policy. |
| `release_policy_uuid` | UUID | Purpose-bound release policy. |
| `purge_policy_uuid` | UUID | Purge/destruction policy. |
| `audit_policy_uuid` | UUID | Audit evidence policy. |
| `created_transaction_uuid` | UUID | Creating transaction or bootstrap event. |
| `created_local_transaction_id` | uint64 | Creating local MGA transaction ID. |
| `updated_transaction_uuid` | UUID | Last mutating transaction or system event. |
| `updated_local_transaction_id` | uint64 | Last mutating local MGA transaction ID. |
| `catalog_generation_id` | uint64 | Visible catalog generation. |
| `security_epoch` | uint64 | Security epoch for visibility and release. |
| `audit_lineage_ref` | UUID/text | Latest audit lineage anchor or redacted evidence reference. |

## Protected Material Identity

The UUID identifies the protected material, not the raw secret. Versions carry
rotated or replaced protected references. Policy bindings decide who can inspect
metadata, resolve a reference, release a value, purge a version, or include
evidence in support output.

## Lifecycle States

| State | Meaning |
| --- | --- |
| `active` | Material has an active version and can be resolved where policy admits it. |
| `disabled_by_policy` | Metadata exists, but resolution or release is blocked. |
| `retained_no_active_version` | Metadata is retained while no version is active. |
| `purged` | Protected reference reachability has been removed according to purge policy. |
| `quarantined` | Material is fenced because integrity, policy, or compromise state is uncertain. |
| `archived` | Metadata is retained for audit, recovery, or compliance but not ordinary use. |

## Visibility And Mutation

Rows are hidden by default unless the effective principal can inspect protected
material metadata. Visible projections must redact sensitive fields.

Mutation is performed only by engine-managed protected-material lifecycle
operations such as create, add version, rotate, disable, quarantine, purge,
restore metadata, or policy change. Ordinary parser text, driver metadata,
support-bundle generation, diagnostics rendering, and catalog projections must
not directly mutate the base table.

## Example Inspection

```sql
select protected_material_uuid,
       object_class,
       purpose_class,
       lifecycle_state,
       active_version_uuid,
       security_epoch
from sys.catalog.protected_material
order by protected_material_uuid;
```

Returned rows and columns depend on disclosure policy.

## Failure Modes

| Condition | Required Behavior |
| --- | --- |
| Metadata hidden by policy | Return redacted not-visible result or denied diagnostic. |
| Active version missing | Refuse resolution and report retained/no-active-version state where visible. |
| Lifecycle state disabled | Deny release or resolution. |
| Quarantined material | Fence release and require operator or policy action. |
| Purged material requested | Return purged-state diagnostic without raw reference data. |
| Security epoch stale | Reauthorize and rebind before release or rendering. |
| Support output attempts raw value | Deny or redact; this is a proof failure if leaked. |

## Verification Checklist

Proof should demonstrate:

- raw protected values are absent from ordinary catalog projections;
- hidden metadata does not leak through errors;
- active version selection uses MGA-visible catalog state;
- release requires release policy and audit evidence;
- purge removes protected reference reachability without deleting required audit
  evidence;
- lifecycle states gate resolution and release;
- security epoch changes invalidate cached protected-material handles.
