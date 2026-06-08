# sys.catalog.type_descriptor Catalog Reference

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `catalog_sys_catalog_type_descriptor`


## Role

`sys.catalog.type_descriptor` is a system catalog surface. It records durable metadata used by the binder, engine verifier, optimizer, security layer, support diagnostics, bridge rendering, or transaction model.

Catalog rows are not parser authority. They are visible through authorized catalog projections, SHOW/DESCRIBE surfaces, information-style views, or support tooling. Base catalog mutation must go through engine-managed catalog operations.

## Keys And Columns

| Column | Type Family | Requirement |
| --- | --- | --- |
| descriptor_uuid | UUID | Canonical type descriptor identity. |
| canonical_type | enum domain | Canonical carrier family. |
| type_family | enum domain | Scalar, temporal, document, spatial, vector, range, container, opaque, extension, etc. |
| source_type_uuid | nullable UUID | Source catalog type where derived. |
| domain_uuid | nullable UUID | Domain identity when descriptor represents a domain slot. |
| modifier_profile_uuid | nullable UUID | Precision, scale, length, dimension, SRID, timezone, or similar modifiers. |
| charset_uuid | nullable UUID | Text charset dependency. |
| collation_uuid | nullable UUID | Text collation dependency. |
| timezone_policy_uuid | nullable UUID | Temporal interpretation policy. |
| storage_codec_uuid | nullable UUID | Storage codec dependency. |
| comparison_contract_uuid | nullable UUID | Comparison/hash/order contract. |
| capability_uuid | nullable UUID | Capability descriptor. |

## Full Definition Extract

### Catalog Table `sys.catalog.type_descriptor`

Primary key: `descriptor_uuid`

Required columns:

| Column | Type family | Requirement |
| --- | --- | --- |
| `descriptor_uuid` | UUID | Canonical type descriptor identity. |
| `canonical_type` | enum domain | Canonical carrier family. |
| `type_family` | enum domain | Scalar, temporal, document, spatial, vector, range, container, opaque, extension, etc. |
| `source_type_uuid` | nullable UUID | Source catalog type where derived. |
| `domain_uuid` | nullable UUID | Domain identity when descriptor represents a domain slot. |
| `modifier_profile_uuid` | nullable UUID | Precision, scale, length, dimension, SRID, timezone, or similar modifiers. |
| `charset_uuid` | nullable UUID | Text charset dependency. |
| `collation_uuid` | nullable UUID | Text collation dependency. |
| `timezone_policy_uuid` | nullable UUID | Temporal interpretation policy. |
| `storage_codec_uuid` | nullable UUID | Storage codec dependency. |
| `comparison_contract_uuid` | nullable UUID | Comparison/hash/order contract. |
| `capability_uuid` | nullable UUID | Capability descriptor. |

## Operational Boundaries

- Base rows require UUID identity and lifecycle metadata.
- Visibility is policy controlled and may use redaction.
- Derived views must preserve base-row authority and must not become engine identity.
- catalog projections are rendering surfaces only.

## Example Inspection

```sql
select *
from sys.catalog.type_descriptor
limit 20;
```
