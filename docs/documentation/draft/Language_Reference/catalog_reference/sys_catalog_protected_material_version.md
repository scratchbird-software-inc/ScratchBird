# sys.catalog.protected_material_version Catalog Reference

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `catalog_sys_catalog_protected_material_version`


## Role

`sys.catalog.protected_material_version` is a system catalog surface. It records durable metadata used by the binder, engine verifier, optimizer, security layer, support diagnostics, bridge rendering, or transaction model.

Catalog rows are not parser authority. They are visible through authorized catalog projections, SHOW/DESCRIBE surfaces, information-style views, or support tooling. Base catalog mutation must go through engine-managed catalog operations.

## Keys And Columns

| Column | Type Family | Requirement |
| --- | --- | --- |
| protected_material_version_uuid | UUID | Stable version identity. |
| protected_material_uuid | UUID | Owning protected material. |
| version_number | uint64 | Monotonic per material. |
| protected_reference_hash | hash/text | Digest of protected reference metadata; never raw reference text where policy marks it sensitive. |
| envelope_hash | hash/text | Digest of wrapped/enveloped/split metadata. |
| payload_hash | hash/text | Payload integrity hash where a referenced payload exists. |
| storage_class | enum | direct, wrapped, split, external_reference, derived, or redacted. |
| rotation_state | enum | active, rotated, retained, purged, quarantined, or compromised_restricted. |
| valid_from_local_transaction_id | uint64 | MGA transaction ID that makes the version visible. |
| valid_until_local_transaction_id | nullable uint64 | MGA transaction ID that ends active visibility. |
| retention_policy_uuid | UUID | Version retention policy. |
| access_policy_uuid | UUID | Version access policy. |
| release_policy_uuid | UUID | Version release policy. |
| purge_policy_uuid | UUID | Version purge policy. |
| audit_policy_uuid | UUID | Version audit policy. |
| retention_until_epoch_millis | uint64 | Earliest policy-admitted purge time. |
| legal_hold | boolean | Purge refusal flag until cleared by policy. |
| purged | boolean | Protected reference reachability has been removed. |
| catalog_generation_id | uint64 | Visible catalog generation. |
| security_epoch | uint64 | Security policy epoch. |

## Full Definition Extract

### Catalog Table `sys.catalog.protected_material_version`

Primary key: `protected_material_version_uuid`

Unique key: `protected_material_uuid`, `version_number`

Required columns:

| Column | Type family | Requirement |
| --- | --- | --- |
| `protected_material_version_uuid` | UUID | Stable version identity. |
| `protected_material_uuid` | UUID | Owning protected material. |
| `version_number` | uint64 | Monotonic per material. |
| `protected_reference_hash` | hash/text | Digest of protected reference metadata; never raw reference text where policy marks it sensitive. |
| `envelope_hash` | hash/text | Digest of wrapped/enveloped/split metadata. |
| `payload_hash` | hash/text | Payload integrity hash where a referenced payload exists. |
| `storage_class` | enum | direct, wrapped, split, external_reference, derived, or redacted. |
| `rotation_state` | enum | active, rotated, retained, purged, quarantined, or compromised_restricted. |
| `valid_from_local_transaction_id` | uint64 | MGA transaction ID that makes the version visible. |
| `valid_until_local_transaction_id` | nullable uint64 | MGA transaction ID that ends active visibility. |
| `retention_policy_uuid` | UUID | Version retention policy. |
| `access_policy_uuid` | UUID | Version access policy. |
| `release_policy_uuid` | UUID | Version release policy. |
| `purge_policy_uuid` | UUID | Version purge policy. |
| `audit_policy_uuid` | UUID | Version audit policy. |
| `retention_until_epoch_millis` | uint64 | Earliest policy-admitted purge time. |
| `legal_hold` | boolean | Purge refusal flag until cleared by policy. |
| `purged` | boolean | Protected reference reachability has been removed. |
| `catalog_generation_id` | uint64 | Visible catalog generation. |
| `security_epoch` | uint64 | Security policy epoch. |

Mutation rule: add-version closes the previous active version by setting `valid_until_local_transaction_id` to the new version's creating transaction ID. Purge clears protected reference reachability, preserves hashes and audit evidence, and never rewrites MGA finality.

Visibility rule: active resolution selects the highest version whose `valid_from_local_transaction_id` is visible in the caller's MGA snapshot and whose `valid_until_local_transaction_id` is null or greater than that snapshot point.

## Operational Boundaries

- Base rows require UUID identity and lifecycle metadata.
- Visibility is policy controlled and may use redaction.
- Derived views must preserve base-row authority and must not become engine identity.
- catalog projections are rendering surfaces only.

## Example Inspection

```sql
select *
from sys.catalog.protected_material_version
limit 20;
```
