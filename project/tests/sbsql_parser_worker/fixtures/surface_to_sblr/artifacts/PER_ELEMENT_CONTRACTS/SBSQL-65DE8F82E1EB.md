# SBSQL-65DE8F82E1EB — psql_execute_statement

Generated public per-element contract snapshot.

## Identity

| Field | Value |
| --- | --- |
| Surface ID | SBSQL-65DE8F82E1EB |
| Fixed UUID v7 | 019dffbb-f000-75a7-8f9c-fbca9b5d830d |
| Canonical name | psql_execute_statement |
| Surface kind | grammar_production |
| Family | runtime_management |

## Route Contract

| Field | Value |
| --- | --- |
| Source status | native_now |
| Cluster scope | noncluster_or_profile_scoped |
| SBLR operation family | sblr.udr.operation.v3 |
| Diagnostic target | canonical_message_vector_and_parser_rendering;parser_support_udr_required;server_revalidates_generated_sblr_uuid;engine_sql_text_execution_false |
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
| Fixture ID | SBSQL-SURFACE-79FB6863A937 |
| Oracle type | canonical_spec_plus_sblr_matrix |
| Oracle search key | SBSQL-65DE8F82E1EB |
| Expected result summary | expected parse, bind, lower, result shape, diagnostic, and executable/refusal behavior derived from canonical spec and operation matrix |
| Oracle closure status | closed_by_semantic_oracle_authority_gate |

## Boundary

- This snapshot is derived only from tracked public release artifacts.
- SQL text remains parser-side input; engine behavior is reached through the published SBLR/internal-API contract.
- This snapshot carries no implementation, source-tree, absolute, or private canonicalization path.
