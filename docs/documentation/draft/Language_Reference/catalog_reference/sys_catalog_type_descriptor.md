# sys.catalog.type_descriptor Catalog Reference

This page documents the authorized catalog surface that describes canonical
type descriptors. Type descriptors are the durable metadata records that let
the binder, SBLR envelope, executor, optimizer, index layer, stream layer, and
result renderer agree on what a value is.

Generation task: `catalog_sys_catalog_type_descriptor`

Related pages: [Type System Overview](../data_types/type_system_overview.md),
[Conversion Matrix](../data_types/conversion_matrix.md),
[sys.catalog.type_capability](sys_catalog_type_capability.md), and
[sys.catalog.domain_descriptor](sys_catalog_domain_descriptor.md).

## Role

`sys.catalog.type_descriptor` records canonical carrier identity. A type name
such as `uint128`, `varchar(120)`, `timestamp(6) with time zone`, or
`vector<float32,1536>` resolves to a descriptor row or to a domain row that
wraps a descriptor row.

The row is used for:

- literal, parameter, column, variable, routine, stream, and result binding;
- cast and assignment validation;
- row, page, overflow, and stream admission;
- comparison, hashing, ordering, grouping, and index eligibility;
- text charset/collation, temporal timezone, vector dimension, document shape,
  and protected-material behavior;
- cache invalidation when descriptor-affecting metadata changes.

## Keys And Columns

Primary key: `descriptor_uuid`

| Column | Type Family | Requirement |
| --- | --- | --- |
| `descriptor_uuid` | UUID | Durable descriptor identity. This is what SBLR and dependent catalog objects bind to. |
| `canonical_type` | enum domain | Canonical SBsql type name or carrier class. |
| `type_family` | enum domain | Scalar, numeric, text, binary, temporal, document, spatial, vector, graph, collection, protected, opaque, or extension family. |
| `source_type_uuid` | nullable UUID | Descriptor this descriptor derives from, where derivation is represented. |
| `domain_uuid` | nullable UUID | Domain identity when this descriptor is a domain-bound slot projection. |
| `modifier_profile_uuid` | nullable UUID | Precision, scale, length, dimension, spatial reference, timezone, or shape modifier profile. |
| `charset_uuid` | nullable UUID | Character set descriptor for text values. |
| `collation_uuid` | nullable UUID | Collation descriptor for text comparison and index keys. |
| `timezone_policy_uuid` | nullable UUID | Temporal timezone and rendering policy. |
| `storage_codec_uuid` | nullable UUID | Storage/overflow/large-value codec descriptor where applicable. |
| `comparison_contract_uuid` | nullable UUID | Equality, ordering, hashing, canonicalization, and null-comparison contract. |
| `capability_uuid` | nullable UUID | Link to the capability row for this descriptor. |

## Column Semantics

### Descriptor UUID

`descriptor_uuid` is stable identity. Names and aliases can change, but SBLR
payloads, prepared statements, indexes, routines, domains, and result
descriptors use the UUID.

### Canonical Type And Type Family

`canonical_type` is the public carrier name or class. `type_family` controls
which descriptor-specific rules apply. For example:

- numeric descriptors own range, precision, scale, and arithmetic behavior;
- text descriptors own character set and collation;
- temporal descriptors own precision and timezone policy;
- vector descriptors own dimension, element profile, metric, and recheck rules;
- protected descriptors own release and redaction policy.

### Modifier Profile

`modifier_profile_uuid` prevents a type spelling from being the only record of
important modifiers. A descriptor for `decimal(18,2)` must carry precision and
scale. A descriptor for `vector<float32,1536>` must carry element profile and
dimension. A descriptor for `timestamp(6) with time zone` must carry precision
and timezone behavior.

### Charset, Collation, And Timezone

These fields are nullable because not every descriptor uses them. When present,
they are dependencies that can affect comparison, sorting, indexing, rendering,
and cache invalidation.

## Visibility And Mutation

Base rows are engine-owned. Users inspect type descriptors through authorized
catalog projections, `SHOW TYPE`, `DESCRIBE TYPE`, information-style views, or
support diagnostics.

Direct user mutation of `sys.catalog.type_descriptor` is not the DDL API.
Descriptor changes must occur through admitted type, domain, catalog, bootstrap,
or extension lifecycle operations and become visible only through MGA
transaction finality.

## Dependencies And Invalidation

Descriptor changes can invalidate:

- prepared statements;
- compiled routines and triggers;
- domains and compound domain elements;
- table columns and generated columns;
- indexes and statistics;
- casts, operators, aggregates, and window functions;
- backup, restore, replication, migration, bridge, and stream descriptors;
- support-bundle and metadata projections.

When any descriptor-affecting epoch changes, dependent state must rebind,
revalidate, rebuild, or refuse execution.

## Example Inspection

```sql
select descriptor_uuid,
       canonical_type,
       type_family,
       modifier_profile_uuid,
       capability_uuid
from sys.catalog.type_descriptor
order by type_family, canonical_type;
```

## Failure Modes

| Condition | Required Behavior |
| --- | --- |
| Descriptor UUID missing | Bind diagnostic for unresolved descriptor. |
| Descriptor hidden by policy | Redacted not-visible or denied diagnostic. |
| Descriptor family mismatch | Bind/admission diagnostic. |
| Required modifier missing | Descriptor-invalid diagnostic. |
| Charset/collation/timezone dependency missing | Bind or admission diagnostic. |
| Capability row missing where required | Capability diagnostic; do not assume defaults. |
| Stale descriptor epoch | Rebind, invalidate cache, or refuse stale execution. |

## Verification Checklist

Proof should demonstrate:

- every public type spelling resolves to a descriptor UUID;
- descriptor UUIDs are stable across rename or alias changes;
- modifiers are represented by descriptor metadata, not text-only spelling;
- charset, collation, timezone, storage, and comparison dependencies are
  enforced;
- SBLR envelopes carry descriptor references;
- hidden descriptors do not leak through unauthorized projections;
- dependent plans and compiled objects invalidate when descriptor metadata
  changes.
