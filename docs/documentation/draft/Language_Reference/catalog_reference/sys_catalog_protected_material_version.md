# sys.catalog.protected_material_version Catalog Reference

This page documents the authorized catalog surface that records versioned
protected-material references. A version row represents a rotated, replaced,
derived, or retained protected reference without exposing raw protected values.

Generation task: `catalog_sys_catalog_protected_material_version`

Related pages: [sys.catalog.protected_material](sys_catalog_protected_material.md),
[sys.catalog.protected_material_policy_binding](sys_catalog_protected_material_policy_binding.md),
[sys.security.catalog.protected_material_audit_event](sys_security_catalog_protected_material_audit_event.md), and
[Binary, UUID, And Protected Values](../data_types/binary_uuid_and_protected_values.md).

## Role

`sys.catalog.protected_material_version` gives protected material an MGA-visible
version history. Rotation, replacement, quarantine, purge, and retention are
recorded without turning raw secret material into ordinary catalog data.

## Keys And Columns

Primary key: `protected_material_version_uuid`

Unique key: `protected_material_uuid`, `version_number`

| Column | Type Family | Requirement |
| --- | --- | --- |
| `protected_material_version_uuid` | UUID | Stable version identity. |
| `protected_material_uuid` | UUID | Owning protected material. |
| `version_number` | uint64 | Monotonic per protected material. |
| `protected_reference_hash` | hash/text | Digest of protected reference metadata. Must not reveal sensitive reference text. |
| `envelope_hash` | hash/text | Digest of wrapped, enveloped, split, or derived metadata. |
| `payload_hash` | hash/text | Integrity hash for referenced payload where policy admits storing it. |
| `storage_class` | enum | `wrapped`, `split`, `external_reference`, `derived`, `redacted`, or admitted equivalent. |
| `rotation_state` | enum | `active`, `rotated`, `retained`, `purged`, `quarantined`, or `compromised_restricted`. |
| `valid_from_local_transaction_id` | uint64 | MGA transaction ID that makes the version visible. |
| `valid_until_local_transaction_id` | nullable uint64 | MGA transaction ID that ends active visibility. |
| `retention_policy_uuid` | UUID | Version retention policy. |
| `access_policy_uuid` | UUID | Version metadata access policy. |
| `release_policy_uuid` | UUID | Version release policy. |
| `purge_policy_uuid` | UUID | Version purge policy. |
| `audit_policy_uuid` | UUID | Version audit policy. |
| `retention_until_epoch_millis` | uint64 | Earliest policy-admitted purge time. |
| `legal_hold` | boolean | Purge refusal flag until cleared by policy. |
| `purged` | boolean | Protected reference reachability has been removed. |
| `catalog_generation_id` | uint64 | Visible catalog generation. |
| `security_epoch` | uint64 | Security epoch for visibility and release. |

## Version Selection

Active resolution selects the highest version that is visible to the caller's
MGA snapshot, is not ended for that snapshot, and is admitted by policy.

```text
protected material
|
+-- version 1: rotated
+-- version 2: retained
+-- version 3: active
```

The active version can differ by transaction snapshot. A version created in an
uncommitted transaction is not ordinary visible state.

## Rotation And Purge

Adding a version closes the previous active version by recording the ending
transaction boundary. Purge removes protected-reference reachability according
to purge policy while preserving hashes and audit evidence where retention
policy requires it.

Purge must not rewrite transaction finality, erase required audit records, or
return plaintext through diagnostics.

## Visibility And Mutation

Rows are exposed only through authorized projections. Mutation is performed by
engine-managed protected-material lifecycle operations: add version, rotate,
quarantine, retain, purge, restore metadata, or policy change.

## Example Inspection

```sql
select protected_material_version_uuid,
       protected_material_uuid,
       version_number,
       rotation_state,
       valid_from_local_transaction_id,
       valid_until_local_transaction_id,
       purged
from sys.catalog.protected_material_version
where protected_material_uuid = :protected_material_uuid
order by version_number;
```

## Failure Modes

| Condition | Required Behavior |
| --- | --- |
| Version not visible to snapshot | Return not visible or select an older visible version. |
| Active version quarantined | Fence release and return diagnostic. |
| Purged version requested | Return purged-state diagnostic without reference data. |
| Legal hold active | Refuse purge. |
| Hash mismatch | Quarantine or fail closed according to policy. |
| Version gap detected | Refuse resolution until classified. |
| Stale security epoch | Reauthorize before release or metadata rendering. |

## Verification Checklist

Proof should demonstrate:

- version numbers are monotonic per protected material;
- version visibility obeys MGA transaction snapshots;
- rotation closes the previous active version;
- purge removes reachability without leaking raw values;
- legal hold blocks purge;
- hidden versions are redacted from unauthorized projections;
- hash mismatch or version gaps fail closed;
- release and support output use version policy and audit evidence.
