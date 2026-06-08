# sys.catalog.operation_descriptor Catalog Reference

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `catalog_sys_catalog_operation_descriptor`


## Role

`sys.catalog.operation_descriptor` is a system catalog surface. It records durable metadata used by the binder, engine verifier, optimizer, security layer, support diagnostics, bridge rendering, or transaction model.

Catalog rows are not parser authority. They are visible through authorized catalog projections, SHOW/DESCRIBE surfaces, information-style views, or support tooling. Base catalog mutation must go through engine-managed catalog operations.

## Keys And Columns

| Column | Type Family | Requirement |
| --- | --- | --- |
| operation_uuid | UUID | Operation identity. |
| operation_family_uuid | UUID | Family identity. |
| operation_kind | enum domain | compare, hash, arithmetic, text, temporal, document, spatial, vector, aggregate, window, locator, opaque, etc. |
| argument_signature_uuid | UUID | Ordered argument descriptors/domains. |
| result_descriptor_uuid | UUID | Result descriptor. |
| domain_stack_policy_uuid | UUID | Domain preservation/erasure behavior. |
| null_missing_policy_uuid | UUID | Null/missing/default/unknown/error behavior. |
| resource_dependency_set_uuid | nullable UUID | Required resources. |
| security_policy_uuid | UUID | Execution privilege/masking policy. |
| determinism_class | enum domain | deterministic, stable, transaction_stable, statement_stable, volatile, side_effecting. |
| cost_class | enum/domain | Optimizer cost family. |
| index_eligibility_uuid | nullable UUID | Index use policy. |
| implementation_ref_uuid | UUID | Internal/SBLR/C++ UDR/LLVM implementation. |
| fallback_ref_uuid | nullable UUID | Fallback implementation. |
| sys.catalog.operation_family | operation_family_uuid | Operation family registry. |
| sys.catalog.cast_rule | cast_rule_uuid | Source/target/cast class/lossiness/security/implementation. |
| sys.catalog.aggregate_descriptor | aggregate_uuid | Transition/combine/inverse/final/state/serialization/cleanup. |
| sys.catalog.window_descriptor | window_uuid | Frame/order/inverse/state behavior. |
| sys.catalog.overload_set | overload_set_uuid | Candidate operations and ranking/ambiguity policy. |
| sys.catalog.comparison_contract | comparison_contract_uuid | Equality/order/hash/canonicalization contract. |
| sys.catalog.canonicalization_profile | canonicalization_profile_uuid | Normalization behavior and resource dependencies. |
| sys.catalog.index_compatibility | index_compatibility_uuid | Descriptor to index-family compatibility. |
| sys.catalog.type_statistics | statistics_uuid | Descriptor-aware statistics and privacy policy. |
| sys.catalog.driver_type_metadata | driver_metadata_uuid | ODBC/JDBC/.NET/native/SBsql metadata. |
| sys.catalog.backup_restore_type_profile | profile_uuid | Logical/physical backup and restore behavior. |
| sys.catalog.replication_type_profile | profile_uuid | Logical-delta and replication compatibility. |
| sys.catalog.cluster_type_transport_profile | profile_uuid | Cross-node transport/resource/fallback policy. |
| sys.catalog.resource_descriptor | resource_uuid | Resource identity and version. |
| sys.catalog.resource_dependency | dependency_uuid | Dependency graph and invalidation behavior. |
| sys.catalog.diagnostic_descriptor | diagnostic_uuid | Diagnostic/error registry. |
| sys.catalog.security_privilege_descriptor | privilege_uuid | Security privilege registry. |
| sys.catalog.type_conformance_case | case_uuid | Conformance manifest catalog. |

## Full Definition Extract

### Catalog Table `sys.catalog.operation_descriptor`

Primary key: `operation_uuid`

Required columns:

| Column | Type family | Requirement |
| --- | --- | --- |
| `operation_uuid` | UUID | Operation identity. |
| `operation_family_uuid` | UUID | Family identity. |
| `operation_kind` | enum domain | compare, hash, arithmetic, text, temporal, document, spatial, vector, aggregate, window, locator, opaque, etc. |
| `argument_signature_uuid` | UUID | Ordered argument descriptors/domains. |
| `result_descriptor_uuid` | UUID | Result descriptor. |
| `domain_stack_policy_uuid` | UUID | Domain preservation/erasure behavior. |
| `null_missing_policy_uuid` | UUID | Null/missing/default/unknown/error behavior. |
| `resource_dependency_set_uuid` | nullable UUID | Required resources. |
| `security_policy_uuid` | UUID | Execution privilege/masking policy. |
| `determinism_class` | enum domain | deterministic, stable, transaction_stable, statement_stable, volatile, side_effecting. |
| `cost_class` | enum/domain | Optimizer cost family. |
| `index_eligibility_uuid` | nullable UUID | Index use policy. |
| `implementation_ref_uuid` | UUID | Internal/SBLR/C++ UDR/LLVM implementation. |
| `fallback_ref_uuid` | nullable UUID | Fallback implementation. |


### Required companion operation tables

| Table | Primary key | Required purpose |
| --- | --- | --- |
| `sys.catalog.operation_family` | `operation_family_uuid` | Operation family registry. |
| `sys.catalog.cast_rule` | `cast_rule_uuid` | Source/target/cast class/lossiness/security/implementation. |
| `sys.catalog.aggregate_descriptor` | `aggregate_uuid` | Transition/combine/inverse/final/state/serialization/cleanup. |
| `sys.catalog.window_descriptor` | `window_uuid` | Frame/order/inverse/state behavior. |
| `sys.catalog.overload_set` | `overload_set_uuid` | Candidate operations and ranking/ambiguity policy. |


## Optimizer, metadata, and transport tables

| Table | Primary key | Required purpose |
| --- | --- | --- |
| `sys.catalog.comparison_contract` | `comparison_contract_uuid` | Equality/order/hash/canonicalization contract. |
| `sys.catalog.canonicalization_profile` | `canonicalization_profile_uuid` | Normalization behavior and resource dependencies. |
| `sys.catalog.index_compatibility` | `index_compatibility_uuid` | Descriptor to index-family compatibility. |
| `sys.catalog.type_statistics` | `statistics_uuid` | Descriptor-aware statistics and privacy policy. |
| `sys.catalog.driver_type_metadata` | `driver_metadata_uuid` | ODBC/JDBC/.NET/native/SBsql metadata. |
| `sys.catalog.backup_restore_type_profile` | `profile_uuid` | Logical/physical backup and restore behavior. |
| `sys.catalog.replication_type_profile` | `profile_uuid` | Logical-delta and replication compatibility. |
| `sys.catalog.cluster_type_transport_profile` | `profile_uuid` | Cross-node transport/resource/fallback policy. |
| `sys.catalog.resource_descriptor` | `resource_uuid` | Resource identity and version. |
| `sys.catalog.resource_dependency` | `dependency_uuid` | Dependency graph and invalidation behavior. |
| `sys.catalog.diagnostic_descriptor` | `diagnostic_uuid` | Diagnostic/error registry. |
| `sys.catalog.security_privilege_descriptor` | `privilege_uuid` | Security privilege registry. |
| `sys.catalog.type_conformance_case` | `case_uuid` | Conformance manifest catalog. |


## Protected material catalog tables


Protected material catalog tables are local `sys.catalog.*` and `sys.security.catalog.*` authority records. Cluster-wide protected material authority may exist only under a manifest-listed cluster security specification and must not be inferred for standalone databases.

## Operational Boundaries

- Base rows require UUID identity and lifecycle metadata.
- Visibility is policy controlled and may use redaction.
- Derived views must preserve base-row authority and must not become engine identity.
- catalog projections are rendering surfaces only.

## Example Inspection

```sql
select *
from sys.catalog.operation_descriptor
limit 20;
```
