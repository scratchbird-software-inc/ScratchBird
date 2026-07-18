# SBSQL-429DB32D5CC2 — jsonb_insert(document,path,value[,insert_after])

Generated public per-element contract snapshot.

## Identity

| Field | Value |
| --- | --- |
| Surface ID | SBSQL-429DB32D5CC2 |
| Fixed UUID v7 | 019dffbb-f000-7b57-8bf7-4a1d5181f6f3 |
| Canonical name | jsonb_insert(document,path,value[,insert_after]) |
| Surface kind | function |
| Family | expression_runtime |

## Route Contract

| Field | Value |
| --- | --- |
| Source status | native_now |
| Cluster scope | noncluster_or_profile_scoped |
| SBLR operation family | sblr.expression.runtime.v3 |
| Diagnostic target | diagnostic.canonical_message_vector |
| Final acceptance rule | parse_bind_lower_server_engine_diagnostic_and_regression_evidence |
| Closure action | implement_full_route_or_exact_canonical_refusal |

## Release Closure

| Field | Value |
| --- | --- |
| Backlog closure status | e2e_passed |
| Release final status | e2e_passed |
| Release claim | public_sbsql_e2e_implemented |
| Release status | row_evidence_complete |
| Remaining risk | none |

## Semantic Oracle

| Field | Value |
| --- | --- |
| Fixture ID | SBSQL-SURFACE-09CAC88C76CE |
| Oracle type | canonical_spec_plus_sblr_matrix |
| Oracle search key | SBSQL-429DB32D5CC2 |
| Expected result summary | expected parse, bind, lower, result shape, diagnostic, and executable/refusal behavior derived from canonical builtin registry and operation matrix; bounded SBSFC-013R-next scalar JSON/document semantics only |
| Oracle closure status | closed_by_semantic_oracle_authority_gate |

## Boundary

- This snapshot is derived only from tracked public release artifacts.
- SQL text remains parser-side input; engine behavior is reached through the published SBLR/internal-API contract.
- This snapshot carries no implementation, source-tree, absolute, or private canonicalization path.
