# sys.catalog.domain_element Catalog Reference

This page is part of the SBsql Language Reference Manual. It documents the authorized catalog surface for addressable elements inside compound domains.

Generation task: `catalog_sys_catalog_domain_element`

Related pages: [Domain Lifecycle](../syntax_reference/domain.md), [Domains, Casts, And Coercion](../data_types/domains_casts_and_coercion.md), and [sys.catalog.domain_descriptor](sys_catalog_domain_descriptor.md).

## Role

`sys.catalog.domain_element` records the field, path, ordinal, range-bound, map-key, variant-tag, opaque-accessor, or document-pointer metadata for compound domains. It is how ScratchBird represents structured domain members without treating path text as durable authority.

An element row is used by:

- structured value validation;
- path access;
- partial update admission;
- element masking/redaction;
- generated columns derived from domain elements;
- indexes over domain elements;
- routine and UDR argument binding;
- metadata rendering;
- support diagnostics.

Catalog rows are not parser authority. They are visible through authorized projections, `DESCRIBE DOMAIN`, information-style views, or support tooling.

## Keys And Columns

Primary key: `element_uuid`

Unique key: `domain_uuid`, `element_ordinal`

| Column | Type Family | Requirement |
| --- | --- | --- |
| `element_uuid` | UUID | Durable element identity. |
| `domain_uuid` | UUID | Owning compound domain. References `sys.catalog.domain_descriptor.domain_uuid`. |
| `element_ordinal` | unsigned integer | Stable ordinal for positional rendering, row-like domains, arrays, lists, and deterministic metadata output. |
| `element_name_uuid` | nullable UUID | Localized/display name reference for named fields or path segments. |
| `path_segment_kind` | enum domain | Segment family: `field_uuid`, `field_ordinal`, `array_index`, `list_index`, `map_key`, `variant_tag`, `range_lower`, `range_upper`, `set_member`, `opaque_accessor`, or `document_pointer`. |
| `target_descriptor_uuid` | UUID | Carrier descriptor for the element value. |
| `target_domain_uuid` | nullable UUID | Domain descriptor for the element value when the element itself is domain-bound. |
| `nullable_policy_uuid` | UUID | Element-level null policy. |
| `visibility_policy_uuid` | UUID | Element read/disclosure policy. |
| `mutation_policy_uuid` | UUID | Element update and partial-mutation policy. |

## Element Identity

Element names are rendering metadata. The durable identity is `element_uuid`, and the owning relationship is `domain_uuid`.

```text
domain_uuid app.address_value
|
+-- element_uuid street
+-- element_uuid city
+-- element_uuid postal_code
```

Renaming an element or changing its display label must not silently change its identity. If a change alters validation or mutation behavior, dependent expressions, indexes, routines, views, and generated columns must rebind or refuse.

## Path Segment Kinds

| Segment Kind | Meaning |
| --- | --- |
| `field_uuid` | Named field resolved by durable field identity. |
| `field_ordinal` | Positional field in a row-like value. |
| `array_index` | Array element selected by index. |
| `list_index` | List element selected by index. |
| `map_key` | Map value selected by key descriptor. |
| `variant_tag` | Active payload selected by variant tag. |
| `range_lower` | Lower bound of a range value. |
| `range_upper` | Upper bound of a range value. |
| `set_member` | Member element of a set-like value. |
| `opaque_accessor` | Policy-owned accessor for opaque values. |
| `document_pointer` | Document path pointer governed by descriptor and policy. |

## Target Descriptor And Target Domain

Every element has a target carrier descriptor. It may also have a target domain. When both are present, assignment to the element must satisfy both the carrier descriptor and the target domain validation pipeline.

```text
element app.address_value.postal_code
|
+-- target_descriptor_uuid -> varchar(20)
+-- target_domain_uuid     -> app.postal_code_text
```

## Visibility And Mutation

Element policy is separate from whole-value policy.

| Policy | Contract |
| --- | --- |
| Null policy | Determines whether the element may be `null`. |
| Visibility policy | Determines whether the effective user or agent may read or render the element. |
| Mutation policy | Determines whether the element may be inserted, updated, patched, cleared, or modified through partial update syntax. |
| Masking policy | Inherited or referenced through the owning domain. May redact an element even when the compound value is visible. |

## Operational Boundaries

- Direct user mutation of `sys.catalog.domain_element` is not the compound-domain DDL API.
- Element metadata must be transactionally visible and rollback-safe.
- Element path text is resolver input only. It is not durable identity.
- Derived views must preserve base-row authority and must not become engine identity.
- Element changes can invalidate generated columns, indexes, routines, UDR bindings, views, materialized views, support projections, and cached plans.

## Example Inspection

```sql
select element_uuid,
       domain_uuid,
       element_ordinal,
       path_segment_kind,
       target_descriptor_uuid,
       target_domain_uuid
from sys.catalog.domain_element
where domain_uuid = :domain_uuid
order by element_ordinal;
```

Use [Domain Lifecycle](../syntax_reference/domain.md) for supported mutation syntax. This catalog page is for metadata interpretation.
