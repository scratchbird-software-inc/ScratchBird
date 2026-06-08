# sys.catalog.type_capability Catalog Reference

This page documents the authorized catalog surface that records what a type
descriptor can safely do: compare, order, hash, group, index, store, render,
move through streams, participate in backup/replication, and cross trusted
runtime boundaries.

Generation task: `catalog_sys_catalog_type_capability`

Related pages: [sys.catalog.type_descriptor](sys_catalog_type_descriptor.md),
[Type System Overview](../data_types/type_system_overview.md),
[Index Lifecycle](../syntax_reference/index.md), and
[Conversion Matrix](../data_types/conversion_matrix.md).

## Role

`sys.catalog.type_capability` prevents the engine from guessing what a type can
do. A descriptor can exist without being orderable, hashable, indexable,
wire-renderable, stream-safe, or safe for acceleration. Capability rows make
those boundaries explicit.

The row is used by:

- binder overload selection;
- comparison, grouping, sorting, and distinct planning;
- index DDL and DML maintenance;
- optimizer plan selection;
- stream, backup, restore, replication, and migration admission;
- trusted UDR and native acceleration admission;
- protected-material handling and redaction.

## Keys And Columns

Primary key: `capability_uuid`

| Column | Type Family | Requirement |
| --- | --- | --- |
| `capability_uuid` | UUID | Durable capability row identity. |
| `descriptor_uuid` | UUID | Type or domain descriptor governed by this capability row. |
| `comparable` | boolean | Equality operation support. |
| `orderable` | boolean | Total or admitted partial ordering support. |
| `hashable` | boolean | Hash-key support for joins, grouping, lookup, or hash indexes. |
| `groupable` | boolean | `GROUP BY`, `DISTINCT`, and grouping-set eligibility. |
| `indexable` | boolean | At least one index family can use this descriptor. |
| `storable` | boolean | Ordinary row, overflow, or large-value storage is admitted. |
| `wire_renderable` | boolean | Value can be rendered through an admitted client/parser/driver result contract. |
| `backup_safe` | boolean | Logical backup can include this descriptor under policy. |
| `replication_safe` | boolean | Logical replication/change streams can carry this descriptor under policy. |
| `cluster_transport_safe` | boolean | Descriptor has a declared transport profile for gated cluster-provider routes. |
| `udr_safe` | boolean | Descriptor can cross a trusted UDR boundary under policy. |
| `llvm_eligible` | boolean | Descriptor can participate in admitted native acceleration. |
| `protected_value_capable` | boolean | Descriptor can carry protected references or release-controlled material. |
| `element_addressable` | boolean | Descriptor supports element/path addressing through domain or compound-value metadata. |

## Capability Semantics

Capability flags are not privileges. They state whether the descriptor has the
technical and semantic contract needed for an operation. Security and policy
are still checked separately.

| Capability | Does Not Mean |
| --- | --- |
| `comparable` | The user may compare hidden or protected values without authorization. |
| `orderable` | Every index family can order the descriptor. |
| `hashable` | Hashes are stable across descriptor-version changes. |
| `indexable` | A specific index declaration is valid. The index family must also be compatible. |
| `wire_renderable` | Protected material can be released. |
| `backup_safe` | The caller has backup privilege. |
| `replication_safe` | The route has ordering, idempotency, or release authority. |
| `udr_safe` | Any UDR can receive the value. The UDR package still requires trust and policy. |
| `llvm_eligible` | Acceleration is required or always enabled. |

## Index And Optimizer Use

`indexable` is broad. A descriptor can be indexable for one index family and not
another. The optimizer must still consult index compatibility, operation
descriptor, collation, comparison contract, statistics, policy, and exact
recheck requirements.

An index provides candidate evidence. It does not own row visibility, security,
transaction finality, or predicate truth.

## Stream And Transport Use

`backup_safe`, `replication_safe`, and transport-related capability flags state
that the descriptor can be represented in an admitted stream. The stream route
still requires:

- object privileges;
- protected-material release policy;
- stream frame limits;
- ordering and idempotency where required;
- target descriptor compatibility;
- transaction and recovery safety.

## Visibility And Mutation

Base rows are engine-owned and are changed through descriptor, extension,
domain, catalog, or bootstrap lifecycle operations. Users inspect capability
state through authorized projections, `DESCRIBE TYPE`, `SHOW TYPE`, support
diagnostics, or information-style views.

## Example Inspection

```sql
select td.canonical_type,
       tc.comparable,
       tc.orderable,
       tc.hashable,
       tc.groupable,
       tc.indexable,
       tc.storable,
       tc.wire_renderable
from sys.catalog.type_capability tc
join sys.catalog.type_descriptor td
  on td.descriptor_uuid = tc.descriptor_uuid
order by td.canonical_type;
```

## Failure Modes

| Condition | Required Behavior |
| --- | --- |
| Capability row missing for descriptor | Refuse operation that requires capability evidence. |
| Operation requires ordering but `orderable` is false | Bind/admission diagnostic. |
| Hash grouping requires `hashable` but flag is false | Bind/admission diagnostic. |
| Index creation requires an unsupported capability | DDL diagnostic. |
| Stream route requires unsafe descriptor | Stream admission diagnostic. |
| Protected value is wire-rendered without release authority | Denied message vector. |
| Capability epoch stale | Rebind, invalidate, or refuse stale execution. |

## Verification Checklist

Proof should demonstrate:

- capability rows exist for all public descriptors that require operation
  admission;
- non-comparable descriptors cannot be used in equality operations;
- non-orderable descriptors cannot be sorted or ordered by B-tree-style keys;
- non-hashable descriptors cannot be used for hash operations;
- index creation checks descriptor and index-family compatibility;
- stream/backup/replication routes check descriptor capability and policy;
- protected descriptors remain redacted unless release is admitted;
- optimizer plans invalidate when capability metadata changes.
