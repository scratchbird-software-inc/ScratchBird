# sys.catalog.domain_element Catalog Reference

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `catalog_sys_catalog_domain_element`


## Role

`sys.catalog.domain_element` is a system catalog surface. It records durable metadata used by the binder, engine verifier, optimizer, security layer, support diagnostics, bridge rendering, or transaction model.

Catalog rows are not parser authority. They are visible through authorized catalog projections, SHOW/DESCRIBE surfaces, information-style views, or support tooling. Base catalog mutation must go through engine-managed catalog operations.

## Keys And Columns

| Column | Type Family | Requirement |
| --- | --- | --- |
| element_uuid | UUID | Element identity. |
| domain_uuid | UUID | Owning compound domain. |
| element_ordinal | unsigned integer | Stable ordinal where positional. |
| element_name_uuid | nullable UUID | Localized/display name reference. |
| path_segment_kind | enum domain | field_uuid, field_ordinal, array_index, list_index, map_key, variant_tag, range_lower, range_upper, set_member, opaque_accessor, document_pointer. |
| target_descriptor_uuid | UUID | Target type descriptor. |
| target_domain_uuid | nullable UUID | Target domain descriptor. |
| nullable_policy_uuid | UUID | Element null policy. |
| visibility_policy_uuid | UUID | Element read/disclosure policy. |
| mutation_policy_uuid | UUID | Element update policy. |

## Full Definition Extract

### Catalog Table `sys.catalog.domain_element`

Primary key: `element_uuid`

Unique key: `domain_uuid`, `element_ordinal`

Required columns:

| Column | Type family | Requirement |
| --- | --- | --- |
| `element_uuid` | UUID | Element identity. |
| `domain_uuid` | UUID | Owning compound domain. |
| `element_ordinal` | unsigned integer | Stable ordinal where positional. |
| `element_name_uuid` | nullable UUID | Localized/display name reference. |
| `path_segment_kind` | enum domain | field_uuid, field_ordinal, array_index, list_index, map_key, variant_tag, range_lower, range_upper, set_member, opaque_accessor, document_pointer. |
| `target_descriptor_uuid` | UUID | Target type descriptor. |
| `target_domain_uuid` | nullable UUID | Target domain descriptor. |
| `nullable_policy_uuid` | UUID | Element null policy. |
| `visibility_policy_uuid` | UUID | Element read/disclosure policy. |
| `mutation_policy_uuid` | UUID | Element update policy. |


## Donor and capability tables

## Operational Boundaries

- Base rows require UUID identity and lifecycle metadata.
- Visibility is policy controlled and may use redaction.
- Derived views must preserve base-row authority and must not become engine identity.
- Donor compatibility projections are rendering surfaces only.

## Example Inspection

```sql
select *
from sys.catalog.domain_element
limit 20;
```
