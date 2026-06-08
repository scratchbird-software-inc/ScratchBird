# sys.catalog.type_capability Catalog Reference

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `catalog_sys_catalog_type_capability`


## Role

`sys.catalog.type_capability` is a system catalog surface. It records durable metadata used by the binder, engine verifier, optimizer, security layer, support diagnostics, bridge rendering, or transaction model.

Catalog rows are not parser authority. They are visible through authorized catalog projections, SHOW/DESCRIBE surfaces, information-style views, or support tooling. Base catalog mutation must go through engine-managed catalog operations.

## Keys And Columns

| Column | Type Family | Requirement |
| --- | --- | --- |
| capability_uuid | UUID | Capability identity. |
| descriptor_uuid | UUID | Type/domain descriptor. |
| comparable | boolean | Equality support. |
| orderable | boolean | Ordering support. |
| hashable | boolean | Hash support. |
| groupable | boolean | Group/distinct support. |
| indexable | boolean | Any index support. |
| storable | boolean | Ordinary storage support. |
| wire_renderable | boolean | Parser/driver rendering support. |
| backup_safe | boolean | Backup support. |
| replication_safe | boolean | Replication support. |
| cluster_transport_safe | boolean | Cluster transport support. |
| udr_safe | boolean | Trusted C++ UDR boundary support. |
| llvm_eligible | boolean | Native acceleration eligibility. |
| protected_value_capable | boolean | Protected-value handling. |
| element_addressable | boolean | DomainElementPath` support. |

## Full Definition Extract

### Catalog Table `sys.catalog.type_capability`

Primary key: `capability_uuid`

Required columns:

| Column | Type family | Requirement |
| --- | --- | --- |
| `capability_uuid` | UUID | Capability identity. |
| `descriptor_uuid` | UUID | Type/domain descriptor. |
| `comparable` | boolean | Equality support. |
| `orderable` | boolean | Ordering support. |
| `hashable` | boolean | Hash support. |
| `groupable` | boolean | Group/distinct support. |
| `indexable` | boolean | Any index support. |
| `storable` | boolean | Ordinary storage support. |
| `wire_renderable` | boolean | Parser/driver rendering support. |
| `backup_safe` | boolean | Backup support. |
| `replication_safe` | boolean | Replication support. |
| `cluster_transport_safe` | boolean | Cluster transport support. |
| `udr_safe` | boolean | Trusted C++ UDR boundary support. |
| `llvm_eligible` | boolean | Native acceleration eligibility. |
| `protected_value_capable` | boolean | Protected-value handling. |
| `element_addressable` | boolean | `DomainElementPath` support. |


## Operation tables

## Operational Boundaries

- Base rows require UUID identity and lifecycle metadata.
- Visibility is policy controlled and may use redaction.
- Derived views must preserve base-row authority and must not become engine identity.
- Donor compatibility projections are rendering surfaces only.

## Example Inspection

```sql
select *
from sys.catalog.type_capability
limit 20;
```
