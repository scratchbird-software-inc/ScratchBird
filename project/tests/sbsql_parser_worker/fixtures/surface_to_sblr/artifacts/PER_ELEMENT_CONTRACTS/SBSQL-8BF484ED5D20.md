# SBSQL-8BF484ED5D20 — insert_statement

Generated public per-element contract snapshot.

## Identity

| Field | Value |
| --- | --- |
| Surface ID | SBSQL-8BF484ED5D20 |
| Fixed UUID v7 | 019dffbb-f000-7dc6-9af8-0527a3d491fd |
| Canonical name | insert_statement |
| Surface kind | grammar_production |
| Family | dml |

## Route Contract

| Field | Value |
| --- | --- |
| Source status | native_now |
| Cluster scope | noncluster_or_profile_scoped |
| SBLR operation family | sblr.dml.operation.v3 |
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
| Fixture ID | SBSQL-SURFACE-ECB149557A44 |
| Oracle type | canonical_spec_plus_sblr_matrix |
| Oracle search key | SBSQL-8BF484ED5D20 |
| Expected result summary | expected parse, bind, lower, result shape, diagnostic, and executable/refusal behavior derived from canonical spec and operation matrix |
| Oracle closure status | closed_by_semantic_oracle_authority_gate |

## Boundary

- This snapshot is derived only from tracked public release artifacts.
- SQL text remains parser-side input; engine behavior is reached through the published SBLR/internal-API contract.
- This snapshot carries no implementation, source-tree, absolute, or private canonicalization path.
