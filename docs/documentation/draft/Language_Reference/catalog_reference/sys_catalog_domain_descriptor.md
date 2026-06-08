# sys.catalog.domain_descriptor Catalog Reference

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `catalog_sys_catalog_domain_descriptor`


## Role

`sys.catalog.domain_descriptor` is a system catalog surface. It records durable metadata used by the binder, engine verifier, optimizer, security layer, support diagnostics, bridge rendering, or transaction model.

Catalog rows are not parser authority. They are visible through authorized catalog projections, SHOW/DESCRIBE surfaces, information-style views, or support tooling. Base catalog mutation must go through engine-managed catalog operations.

## Keys And Columns

| Column | Type Family | Requirement |
| --- | --- | --- |
| domain_uuid | UUID | Domain identity. |
| domain_kind | enum domain | scalar, compound, array, row, map, range, enum, variant, opaque, donor_emulation, protected_history. |
| base_descriptor_uuid | nullable UUID | Base execution descriptor. |
| base_domain_uuid | nullable UUID | Wrapped base domain. |
| domain_stack_hash | binary/hash | Stable hash of resolved domain stack. |
| nullable_policy_uuid | UUID | Null admission policy. |
| default_expression_uuid | nullable UUID | Default expression or generator. |
| constraint_set_uuid | nullable UUID | Domain constraint set. |
| element_policy_uuid | nullable UUID | Compound element visibility/mutation policy. |
| cast_policy_uuid | nullable UUID | Cast policy set. |
| operation_policy_uuid | nullable UUID | Operation policy set. |
| masking_policy_uuid | nullable UUID | Masking/protected-value policy. |
| donor_family | nullable enum domain | Donor family scope. |
| donor_type_name | nullable text domain | Donor-facing type name. |

## Full Definition Extract

### Catalog Table `sys.catalog.domain_descriptor`

Primary key: `domain_uuid`

Required columns:

| Column | Type family | Requirement |
| --- | --- | --- |
| `domain_uuid` | UUID | Domain identity. |
| `domain_kind` | enum domain | scalar, compound, array, row, map, range, enum, variant, opaque, donor_emulation, protected_history. |
| `base_descriptor_uuid` | nullable UUID | Base execution descriptor. |
| `base_domain_uuid` | nullable UUID | Wrapped base domain. |
| `domain_stack_hash` | binary/hash | Stable hash of resolved domain stack. |
| `nullable_policy_uuid` | UUID | Null admission policy. |
| `default_expression_uuid` | nullable UUID | Default expression or generator. |
| `constraint_set_uuid` | nullable UUID | Domain constraint set. |
| `element_policy_uuid` | nullable UUID | Compound element visibility/mutation policy. |
| `cast_policy_uuid` | nullable UUID | Cast policy set. |
| `operation_policy_uuid` | nullable UUID | Operation policy set. |
| `masking_policy_uuid` | nullable UUID | Masking/protected-value policy. |
| `donor_family` | nullable enum domain | Donor family scope. |
| `donor_type_name` | nullable text domain | Donor-facing type name. |

## Operational Boundaries

- Base rows require UUID identity and lifecycle metadata.
- Visibility is policy controlled and may use redaction.
- Derived views must preserve base-row authority and must not become engine identity.
- Donor compatibility projections are rendering surfaces only.

## Example Inspection

```sql
select *
from sys.catalog.domain_descriptor
limit 20;
```
