# SBSQL-ED484B8BAA9E — LISTAGG(expr[,sep][ONOVERFLOW...])WITHINGROUP(ORDERBY...)

Generated public per-element contract snapshot.

## Identity

| Field | Value |
| --- | --- |
| Surface ID | SBSQL-ED484B8BAA9E |
| Fixed UUID v7 | 019dffbb-f000-7de7-8669-fc0e78b81cd6 |
| Canonical name | LISTAGG(expr[,sep][ONOVERFLOW...])WITHINGROUP(ORDERBY...) |
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
| Fixture ID | SBSQL-SURFACE-CE7083BC3834 |
| Oracle type | promotion_or_canonical_refusal_decision |
| Oracle search key | SBSQL-ED484B8BAA9E |
| Expected result summary | owning slice must resolve expected success/refusal before fixture emission; source authority is preassigned here |
| Oracle closure status | closed_by_semantic_oracle_authority_gate |

## Boundary

- This snapshot is derived only from tracked public release artifacts.
- SQL text remains parser-side input; engine behavior is reached through the published SBLR/internal-API contract.
- This snapshot carries no implementation, source-tree, absolute, or private canonicalization path.
