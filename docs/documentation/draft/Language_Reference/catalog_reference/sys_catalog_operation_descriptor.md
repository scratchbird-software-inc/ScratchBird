# sys.catalog.operation_descriptor Catalog Reference

This page documents the authorized catalog surface that describes SBsql/SBLR
operations: argument descriptors, result descriptors, determinism, null and
missing behavior, domain preservation, implementation routing, security, cost,
and index eligibility.

Generation task: `catalog_sys_catalog_operation_descriptor`

Related pages: [Parser To SBLR Pipeline](../core_paradigms/parser_to_sblr_pipeline.md),
[Operator Type Result Matrix](../syntax_reference/operator_type_result_matrix.md),
[Conversion Matrix](../data_types/conversion_matrix.md), and
[sys.catalog.type_descriptor](sys_catalog_type_descriptor.md).

## Role

`sys.catalog.operation_descriptor` is the metadata bridge between a bound
expression or statement and the executable SBLR operation. It tells the binder
and server admission what argument shape is valid, what result shape is
produced, how null/missing values behave, whether domains are preserved or
erased, whether an index can be used, and which implementation route is
admitted.

The page covers the public interpretation of the table. It does not expose
private implementation entry points.

## Keys And Columns

Primary key: `operation_uuid`

| Column | Type Family | Requirement |
| --- | --- | --- |
| `operation_uuid` | UUID | Durable operation identity used by SBLR routing. |
| `operation_family_uuid` | UUID | Operation family identity for grouping, inspection, and admission. |
| `operation_kind` | enum domain | Compare, hash, arithmetic, text, temporal, document, spatial, vector, aggregate, window, locator, management, stream, opaque, or other admitted family. |
| `argument_signature_uuid` | UUID | Ordered argument descriptors, domain rules, parameter modes, and variadic behavior. |
| `result_descriptor_uuid` | UUID | Descriptor of the scalar, row, cursor, stream, command, or diagnostic result. |
| `domain_stack_policy_uuid` | UUID | Domain preservation, erasure, or common-domain derivation behavior. |
| `null_missing_policy_uuid` | UUID | Null, missing, default, unknown, and error behavior. |
| `resource_dependency_set_uuid` | nullable UUID | Required collation, timezone, metric, tokenizer, spatial reference, stream, package, or provider resources. |
| `security_policy_uuid` | UUID | Execution privilege, masking, protected-material, or disclosure policy. |
| `determinism_class` | enum domain | `deterministic`, `stable`, `transaction_stable`, `statement_stable`, `volatile`, or `side_effecting`. |
| `cost_class` | enum/domain | Optimizer cost family and planning hints. |
| `index_eligibility_uuid` | nullable UUID | Index compatibility and exact-recheck contract. |
| `implementation_ref_uuid` | UUID | Public operation route identity for SBLR dispatch. |
| `fallback_ref_uuid` | nullable UUID | Alternate operation route used only when admitted. |

## Operation Identity

`operation_uuid` is the executable identity after binding. Names such as
operator symbols, function names, aggregate names, and statement-family labels
are resolver input. SBLR routes by operation identity and descriptor shape.

## Argument And Result Signatures

An operation's argument signature records:

- number of arguments;
- argument order;
- descriptor or domain expected for each argument;
- parameter mode where applicable;
- variadic behavior;
- null and missing handling;
- implicit assignment conversions allowed before execution;
- protected-material restrictions;
- result-shape derivation rules.

The result descriptor can be scalar, row, cursor, stream, command completion,
diagnostic, or another descriptor-bound shape.

## Determinism

| Determinism Class | Meaning |
| --- | --- |
| `deterministic` | Same arguments and same descriptor/resource versions produce the same result. |
| `stable` | Stable within a catalog/resource epoch. |
| `transaction_stable` | Stable within one transaction context. |
| `statement_stable` | Stable within one admitted statement. |
| `volatile` | May change between calls and cannot be freely reordered or folded. |
| `side_effecting` | Performs state-changing or externally visible work and requires stricter admission. |

Determinism affects constant folding, generated columns, indexes, materialized
views, plans, cacheability, and support diagnostics.

## Null, Missing, And Domain Policies

`null_missing_policy_uuid` separates SQL null, document missing, default value,
unknown, empty, and error behavior. An operation must not silently collapse
these states unless its descriptor says so.

`domain_stack_policy_uuid` controls whether a domain result is preserved,
erased to its carrier, or converted to a common domain. Domain behavior must be
explicit because domains can carry constraints, masks, and operation policies.

## Index Eligibility

`index_eligibility_uuid` says whether an operation can use an index and what
recheck is required.

Index eligibility can depend on:

- operation kind;
- argument descriptors;
- collation;
- comparison contract;
- temporal precision;
- vector metric;
- spatial reference;
- document path;
- graph traversal descriptor;
- exact-recheck requirement;
- protected-material and policy state.

An index is candidate evidence. The executor still rechecks MGA visibility,
security, predicate truth, descriptor compatibility, and result shape.

## Security And Protected Material

`security_policy_uuid` binds the operation to required privileges and
redaction/release policy. Operations that render values, export streams, open
bridge routes, inspect metadata, or release protected material require explicit
policy admission.

An operation can be syntactically valid and descriptor-valid but still refused
by security policy.

## Example Inspection

```sql
select operation_uuid,
       operation_kind,
       argument_signature_uuid,
       result_descriptor_uuid,
       determinism_class
from sys.catalog.operation_descriptor
where operation_kind in ('arithmetic', 'text', 'aggregate')
order by operation_kind, operation_uuid;
```

## Visibility And Mutation

Base rows are engine-owned and created or updated by catalog, type, function,
operator, aggregate, window, extension, package, or bootstrap lifecycle
operations. User statements inspect operation metadata through authorized
projections, `DESCRIBE FUNCTION`, `DESCRIBE OPERATOR`, `SHOW FUNCTIONS`,
operator documentation, or support diagnostics.

## Dependencies And Invalidation

Operation descriptor changes can invalidate:

- prepared expressions and statements;
- generated columns;
- indexes and statistics;
- views and materialized views;
- compiled routines and triggers;
- cast and overload decisions;
- optimizer plans;
- stream and bridge operation routes;
- support-bundle projections.

## Failure Modes

| Condition | Required Behavior |
| --- | --- |
| Operation name has no visible overload | Bind diagnostic. |
| Multiple overloads rank equally | Ambiguity diagnostic. |
| Argument descriptor mismatch | Bind diagnostic. |
| Result descriptor missing | Catalog diagnostic; operation cannot execute. |
| Null/missing policy absent | Admission diagnostic. |
| Operation is not deterministic but used in deterministic context | DDL or bind diagnostic. |
| Index eligibility missing for indexed expression | DDL or planning diagnostic. |
| Implementation route unavailable | Unsupported or unavailable capability message vector. |
| Security policy denies operation | Denied message vector. |
| Operation epoch stale | Rebind or refuse cached execution. |

## Verification Checklist

Proof should demonstrate:

- operation names resolve to operation UUIDs only after descriptor-aware
  overload selection;
- ambiguous overloads are refused;
- argument and result descriptors match the documented operation contract;
- null, missing, and domain policies are enforced;
- non-deterministic operations are refused in deterministic-only contexts;
- index eligibility requires exact recheck where applicable;
- security policy can deny an otherwise valid operation;
- stale operation metadata invalidates dependent plans and compiled objects;
- SBLR envelopes route by operation identity, not by source text.
