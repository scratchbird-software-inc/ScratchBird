# sys.catalog.protected_material_policy_binding Catalog Reference

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `catalog_sys_catalog_protected_material_policy_binding`


## Role

`sys.catalog.protected_material_policy_binding` is a system catalog surface. It records durable metadata used by the binder, engine verifier, optimizer, security layer, support diagnostics, bridge rendering, or transaction model.

Catalog rows are not parser authority. They are visible through authorized catalog projections, SHOW/DESCRIBE surfaces, information-style views, or support tooling. Base catalog mutation must go through engine-managed catalog operations.

## Keys And Columns

| Column | Type Family | Requirement |
| --- | --- | --- |
| binding_uuid | UUID | Stable binding identity. |
| protected_material_uuid | UUID | Bound material. |
| protected_material_version_uuid | nullable UUID | Bound version, or null for material-level binding. |
| policy_uuid | UUID | Bound policy. |
| policy_kind | enum | retention, access, release, purge, audit, diagnostic, redaction, backup_restore, or support_bundle. |
| diagnostic_state | enum/text | active, disabled_by_policy, security_redacted, purge_blocked, release_blocked, or archived. |
| catalog_generation_id | uint64 | Visible catalog generation. |
| security_epoch | uint64 | Security epoch. |

## Full Definition Extract

### Catalog Table `sys.catalog.protected_material_policy_binding`

Primary key: `binding_uuid`

Required columns:

| Column | Type family | Requirement |
| --- | --- | --- |
| `binding_uuid` | UUID | Stable binding identity. |
| `protected_material_uuid` | UUID | Bound material. |
| `protected_material_version_uuid` | nullable UUID | Bound version, or null for material-level binding. |
| `policy_uuid` | UUID | Bound policy. |
| `policy_kind` | enum | retention, access, release, purge, audit, diagnostic, redaction, backup_restore, or support_bundle. |
| `diagnostic_state` | enum/text | active, disabled_by_policy, security_redacted, purge_blocked, release_blocked, or archived. |
| `catalog_generation_id` | uint64 | Visible catalog generation. |
| `security_epoch` | uint64 | Security epoch. |

## Operational Boundaries

- Base rows require UUID identity and lifecycle metadata.
- Visibility is policy controlled and may use redaction.
- Derived views must preserve base-row authority and must not become engine identity.
- catalog projections are rendering surfaces only.

## Example Inspection

```sql
select *
from sys.catalog.protected_material_policy_binding
limit 20;
```
