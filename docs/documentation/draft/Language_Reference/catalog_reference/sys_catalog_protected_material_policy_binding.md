# sys.security.protected_material_policy_binding Catalog Reference

This page documents the authorized catalog surface that binds protected
material or protected-material versions to retention, access, release, purge,
audit, diagnostic, redaction, backup, restore, replication, migration, and
support policies.

Generation task: `catalog_sys_security_protected_material_policy_binding`

Related pages: [sys.security.protected_material_catalog](sys_catalog_protected_material.md),
[sys.security.protected_material_version](sys_catalog_protected_material_version.md),
[sys.security.protected_material_audit](sys_security_catalog_protected_material_audit_event.md), and
[Security And Sandboxing](../core_paradigms/security_and_sandboxing.md).

## Role

`sys.security.protected_material_policy_binding` records which policies control
protected material. A material can have material-level policies and
version-level policies. Version-level policy can narrow or override behavior
only where the policy model admits it.

The table is used before metadata rendering, reference resolution, release,
export, backup, restore, replication, purge, diagnostics, and support-bundle
generation.

## Keys And Columns

Primary key: `binding_uuid`

| Column | Type Family | Requirement |
| --- | --- | --- |
| `binding_uuid` | UUID | Stable binding identity. |
| `protected_material_uuid` | UUID | Bound protected material. |
| `protected_material_version_uuid` | nullable UUID | Bound version, or null for material-level binding. |
| `policy_uuid` | UUID | Policy object that controls the behavior. |
| `policy_kind` | enum/text | `retention`, `access`, `release`, `purge`, or `audit`. |
| `diagnostic_state` | enum/text | Engine-assigned diagnostic policy state for the binding. |
| `catalog_generation_id` | uint64 | Visible catalog generation. |
| `security_epoch` | uint64 | Security epoch for visibility and release. |

## Policy Kinds

| Policy Kind | Controls |
| --- | --- |
| `retention` | How long metadata, versions, hashes, and audit evidence are kept. |
| `access` | Who can inspect metadata or resolve references. |
| `release` | Who can obtain raw material for a specific admitted purpose. |
| `purge` | When protected-reference reachability can be destroyed. |
| `audit` | Which events must be recorded and how long evidence is retained. |

## Resolution Rules

When protected material is accessed:

1. bind protected material UUID;
2. select visible version where needed;
3. load material-level policy bindings;
4. load version-level policy bindings;
5. combine policies according to policy precedence;
6. apply security epoch and transaction visibility;
7. admit, redact, deny, or quarantine the request;
8. record audit evidence where policy requires it.

Missing required policy is a refusal, not permission to proceed.

## Visibility And Mutation

Rows are visible only through authorized projections. Base rows are created or
changed by protected-material lifecycle and security-policy operations.

Changing a binding advances the security epoch and invalidates cached protected
handles, stream routes, support projections, metadata projections, and any
compiled or prepared operation that depended on the prior binding.

## Example Inspection

```sql
select protected_material_uuid,
       protected_material_version_uuid,
       policy_kind,
       diagnostic_state,
       security_epoch
from sys.security.protected_material_policy_binding
where protected_material_uuid = :protected_material_uuid
order by policy_kind;
```

## Failure Modes

| Condition | Required Behavior |
| --- | --- |
| Required policy missing | Refuse protected-material operation. |
| Binding hidden by policy | Redact or hide binding metadata. |
| Binding disabled | Deny the controlled behavior. |
| Release blocked | Deny release and emit release diagnostic where visible. |
| Purge blocked | Refuse purge and preserve reachability. |
| Conflicting policies | Fail closed unless precedence resolves them. |
| Stale security epoch | Reauthorize and invalidate cached state. |

## Verification Checklist

Proof should demonstrate:

- material-level and version-level policies are both considered;
- missing required policy fails closed;
- release, purge, support, backup, replication, and migration behavior depend on
  explicit bindings;
- binding changes advance security epoch and invalidate dependent state;
- unauthorized users cannot infer hidden policy details;
- diagnostics are redacted according to diagnostic/redaction policy;
- audit evidence is recorded where policy requires it.
