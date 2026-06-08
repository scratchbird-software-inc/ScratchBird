# sys.catalog.domain_descriptor Catalog Reference

This page is part of the SBsql Language Reference Manual. It documents the authorized catalog surface that describes domain identity, domain stacks, validation policy, cast policy, operation policy, and masking policy.

Generation task: `catalog_sys_catalog_domain_descriptor`

Related pages: [Domain Lifecycle](../syntax_reference/domain.md), [Domains, Casts, And Coercion](../data_types/domains_casts_and_coercion.md), [sys.catalog.domain_element](sys_catalog_domain_element.md), [sys.catalog.type_descriptor](sys_catalog_type_descriptor.md), and [sys.catalog.type_capability](sys_catalog_type_capability.md).

## Role

`sys.catalog.domain_descriptor` records durable metadata for every domain. A domain descriptor is the catalog authority for:

- the domain UUID;
- the base carrier descriptor;
- the optional parent domain;
- the resolved domain stack;
- null policy;
- defaults;
- constraints;
- element policy;
- cast policy;
- operation policy;
- masking/protected-value policy;
- SBsql alias rendering metadata.

Catalog rows are not parser authority. They are visible through authorized catalog projections, `SHOW DOMAIN`, `DESCRIBE DOMAIN`, information-style views, or support tooling. Base catalog mutation must go through engine-managed domain lifecycle operations.

## Keys And Columns

Primary key: `domain_uuid`

| Column | Type Family | Requirement |
| --- | --- | --- |
| `domain_uuid` | UUID | Durable domain identity. This value is what dependent columns, routines, indexes, views, and SBLR descriptors bind to. |
| `domain_kind` | enum domain | Domain family: `scalar`, `compound`, `array`, `row`, `map`, `range`, `enum`, `variant`, `opaque`, `alias_profile`, or `protected_history`. |
| `base_descriptor_uuid` | nullable UUID | Canonical carrier descriptor used for physical representation and primitive operations. Required unless the domain kind is represented entirely through a base domain or policy-owned opaque binding. |
| `base_domain_uuid` | nullable UUID | Parent domain UUID for domain-over-domain stacks. |
| `domain_stack_hash` | binary/hash | Stable hash of the resolved base descriptor plus parent-domain chain and this domain's validation identity. |
| `nullable_policy_uuid` | UUID | Policy that admits or refuses `null` for values of this domain. |
| `default_expression_uuid` | nullable UUID | Bound default expression used when no more specific assignment-site default exists. |
| `constraint_set_uuid` | nullable UUID | Set of named domain checks evaluated with the `VALUE` pseudo-value. |
| `element_policy_uuid` | nullable UUID | Compound-domain element policy set. Used with `sys.catalog.domain_element`. |
| `cast_policy_uuid` | nullable UUID | Policy governing implicit assignment, explicit casts, lossiness, domain preservation, and domain erasure. |
| `operation_policy_uuid` | nullable UUID | Policy governing comparison, hashing, ordering, arithmetic, text, temporal, document, vector, spatial, opaque, aggregate, and index behavior. |
| `masking_policy_uuid` | nullable UUID | Redaction, protected-value, support-bundle, or projection masking policy. |
| `source_family` | nullable enum domain | SBsql alias/rendering family used when a domain has a policy-owned alternate surface name. |
| `source_type_name` | nullable text domain | SBsql-facing type or alias spelling rendered by authorized metadata views. |

## Column Semantics

### Identity

`domain_uuid` is stable across rename. The resolver name can change, but stored descriptors, SBLR payloads, indexes, routines, views, and table columns remain bound to the UUID.

### Base Descriptor And Base Domain

A domain may wrap a canonical descriptor directly:

```text
app.nonnegative_money
|
+-- base_descriptor_uuid -> decimal(18,2)
```

It may also wrap another domain:

```text
app.customer_label
|
+-- base_domain_uuid -> app.nonblank_text
    |
    +-- base_descriptor_uuid -> varchar(200)
```

The binder resolves this chain before execution and records the result in the domain stack.

### Domain Stack Hash

`domain_stack_hash` detects whether the resolved chain used by a prepared statement, stored routine, generated column, index, or view is still valid. If any domain in the chain changes in a way that affects validation or operations, dependent compiled metadata must rebind or refuse.

### Policy UUIDs

Policy UUID columns point to policy records outside this table. The domain descriptor stores references so the binder and engine can apply a single authority model for validation, casting, comparison, indexing, masking, and element mutation.

## Dependency Behavior

The following objects normally depend on `domain_uuid`:

- table columns;
- generated columns;
- defaults and constraints;
- indexes and index expressions;
- views and materialized views;
- procedure/function parameters and returns;
- triggers;
- UDR bindings;
- cast descriptors;
- operation descriptors;
- support-bundle and metadata projections.

Changing a domain descriptor can invalidate any of those dependencies. The engine must either refresh them, revalidate them, or refuse the change.

## Operational Boundaries

- Base catalog rows require UUID identity and transaction lifecycle metadata.
- Direct user mutation of `sys.catalog.domain_descriptor` is not the domain DDL API.
- Visibility is policy controlled and may use redaction.
- Derived views must preserve base-row authority and must not become engine identity.
- `SHOW DOMAIN` and `DESCRIBE DOMAIN` are authorized projections over this table and related policy tables.
- DDL changes become visible only through MGA transaction finality.

## Example Inspection

```sql
select domain_uuid,
       domain_kind,
       base_descriptor_uuid,
       base_domain_uuid,
       domain_stack_hash
from sys.catalog.domain_descriptor
order by domain_kind, domain_uuid;
```

Use [Domain Lifecycle](../syntax_reference/domain.md) for supported mutation syntax. This catalog page is for metadata interpretation.
