# SBSQL-6C4FC1E18B25 — sum([DISTINCT]expr)

Generated public per-element contract snapshot.

## Identity

| Field | Value |
| --- | --- |
| Surface ID | SBSQL-6C4FC1E18B25 |
| Fixed UUID v7 | 019dffbb-f000-7c27-9d8f-7591f855fbcd |
| Canonical name | sum([DISTINCT]expr) |
| Surface kind | function |
| Family | expression_runtime |

## Route Contract

| Field | Value |
| --- | --- |
| Source status | native_now |
| Cluster scope | noncluster_or_profile_scoped |
| SBLR operation family | sblr.expression.runtime.v3 |
| Diagnostic target | canonical_message_vector_and_parser_rendering |
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
| Fixture ID | SBSQL-SURFACE-3A3EC9746D70 |
| Oracle type | canonical_spec_plus_sblr_matrix |
| Oracle search key | SBSQL-6C4FC1E18B25 |
| Expected result summary | expected parse, bind, lower, result shape, diagnostic, and executable/refusal behavior derived from canonical spec and operation matrix |
| Oracle closure status | closed_by_semantic_oracle_authority_gate |

## Boundary

- This snapshot is derived only from tracked public release artifacts.
- SQL text remains parser-side input; engine behavior is reached through the published SBLR/internal-API contract.
- This snapshot carries no implementation, source-tree, absolute, or private canonicalization path.
