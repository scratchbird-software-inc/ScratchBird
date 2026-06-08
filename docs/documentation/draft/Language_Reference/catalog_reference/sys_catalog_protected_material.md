# sys.catalog.protected_material Catalog Reference

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `catalog_sys_catalog_protected_material`


## Role

`sys.catalog.protected_material` is a system catalog surface. It records durable metadata used by the binder, engine verifier, optimizer, security layer, support diagnostics, bridge rendering, or transaction model.

Catalog rows are not parser authority. They are visible through authorized catalog projections, SHOW/DESCRIBE surfaces, information-style views, or support tooling. Base catalog mutation must go through engine-managed catalog operations.

## Keys And Columns

| Column | Type Family | Requirement |
| --- | --- | --- |
| protected_material_uuid | UUID | Stable material identity. |
| object_class | enum/text | protected_material` or admitted protected material subclass. |
| owner_scope_uuid | UUID | Owning database, schema, security authority, package, user, role, or system scope. |
| purpose_class | enum/text | Purpose class controlling default release purpose. |
| storage_class | enum | direct, wrapped, split, external_reference, derived, or redacted; direct storage still forbids plaintext. |
| active_version_uuid | nullable UUID | Active version visible at the current MGA catalog generation. |
| lifecycle_state | enum | active, disabled_by_policy, retained_no_active_version, purged, quarantined, or archived. |
| retention_policy_uuid | UUID | Retention policy. |
| access_policy_uuid | UUID | Metadata and read visibility policy. |
| release_policy_uuid | UUID | Purpose-bound release policy. |
| purge_policy_uuid | UUID | Purge/destruction policy. |
| audit_policy_uuid | UUID | Audit evidence policy. |
| created_transaction_uuid | UUID | Creating MGA transaction or bootstrap event. |
| created_local_transaction_id | uint64 | Creating local MGA transaction ID. |
| updated_transaction_uuid | UUID | Last mutating MGA transaction or system event. |
| updated_local_transaction_id | uint64 | Last mutating local MGA transaction ID. |
| catalog_generation_id | uint64 | Visible catalog generation. |
| security_epoch | uint64 | Security policy epoch for visibility and release. |
| audit_lineage_ref | UUID/text | Latest protected material audit anchor. |

## Full Definition Extract

### Catalog Table `sys.catalog.protected_material`

Primary key: `protected_material_uuid`

Required columns:

| Column | Type family | Requirement |
| --- | --- | --- |
| `protected_material_uuid` | UUID | Stable material identity. |
| `object_class` | enum/text | `protected_material` or admitted protected material subclass. |
| `owner_scope_uuid` | UUID | Owning database, schema, security authority, package, user, role, or system scope. |
| `purpose_class` | enum/text | Purpose class controlling default release purpose. |
| `storage_class` | enum | direct, wrapped, split, external_reference, derived, or redacted; direct storage still forbids plaintext. |
| `active_version_uuid` | nullable UUID | Active version visible at the current MGA catalog generation. |
| `lifecycle_state` | enum | active, disabled_by_policy, retained_no_active_version, purged, quarantined, or archived. |
| `retention_policy_uuid` | UUID | Retention policy. |
| `access_policy_uuid` | UUID | Metadata and read visibility policy. |
| `release_policy_uuid` | UUID | Purpose-bound release policy. |
| `purge_policy_uuid` | UUID | Purge/destruction policy. |
| `audit_policy_uuid` | UUID | Audit evidence policy. |
| `created_transaction_uuid` | UUID | Creating MGA transaction or bootstrap event. |
| `created_local_transaction_id` | uint64 | Creating local MGA transaction ID. |
| `updated_transaction_uuid` | UUID | Last mutating MGA transaction or system event. |
| `updated_local_transaction_id` | uint64 | Last mutating local MGA transaction ID. |
| `catalog_generation_id` | uint64 | Visible catalog generation. |
| `security_epoch` | uint64 | Security policy epoch for visibility and release. |
| `audit_lineage_ref` | UUID/text | Latest protected material audit anchor. |

Mutation rule: only engine security/catalog APIs may insert or update rows. Parser SQL text, SBsql metadata, driver metadata, support bundle export, cloud provider status, Kubernetes status, CDN streams, SBsql, and WAL-like journals must not mutate this table.

Visibility rule: rows are hidden by default unless the effective user can see protected material metadata under access policy. Visible rows expose only redacted metadata through presentation views.

## Operational Boundaries

- Base rows require UUID identity and lifecycle metadata.
- Visibility is policy controlled and may use redaction.
- Derived views must preserve base-row authority and must not become engine identity.
- catalog projections are rendering surfaces only.

## Example Inspection

```sql
select *
from sys.catalog.protected_material
limit 20;
```
