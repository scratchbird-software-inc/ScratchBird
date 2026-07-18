# SBSQL-EF81CD34B2B5 — ddl_relational_stmt

Generated public per-element contract snapshot.

## Identity

| Field | Value |
| --- | --- |
| Surface ID | SBSQL-EF81CD34B2B5 |
| Fixed UUID v7 | 019dffbb-f000-7696-8363-3616957bc6e9 |
| Canonical name | ddl_relational_stmt |
| Surface kind | grammar_production |
| Family | general |

## Route Contract

| Field | Value |
| --- | --- |
| Source status | native_now |
| Cluster scope | noncluster_or_profile_scoped |
| SBLR operation family | sblr.general.operation.v3 |
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
| Fixture ID | SBSQL-SURFACE-4152539E0F4C |
| Oracle type | canonical_spec_plus_sblr_matrix |
| Oracle search key | SBSQL-EF81CD34B2B5 |
| Expected result summary | expected parse, bind, lower, result shape, diagnostic, and executable/refusal behavior derived from canonical spec and operation matrix |
| Oracle closure status | closed_by_semantic_oracle_authority_gate |

## Boundary

- This snapshot is derived only from tracked public release artifacts.
- SQL text remains parser-side input; engine behavior is reached through the published SBLR/internal-API contract.
- This snapshot carries no implementation, source-tree, absolute, or private canonicalization path.
