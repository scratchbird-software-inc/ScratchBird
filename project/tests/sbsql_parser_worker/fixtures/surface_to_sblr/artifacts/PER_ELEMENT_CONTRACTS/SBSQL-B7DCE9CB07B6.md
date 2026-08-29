# SBSQL-B7DCE9CB07B6 — cypher_load_csv

Generated public per-element contract snapshot.

## Identity

| Field | Value |
| --- | --- |
| Surface ID | SBSQL-B7DCE9CB07B6 |
| Fixed UUID v7 | 019dffbb-f000-7d28-8027-6105ebb09074 |
| Canonical name | cypher_load_csv |
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
| Canonical diagnostic precedence | SBLR.OPCODE_INVALID;SBLR.OPERAND_INVALID;SECURITY.ACCESS_DENIED;MGA.TRANSACTION_INVALID;MGA.AUTHORITY_MISMATCH;CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN;PROCESS.CANCELLED;SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING;SBLR.OPERATION_UNSUPPORTED |

## Release Closure

| Field | Value |
| --- | --- |
| Backlog closure status | e2e_passed |
| Release final status | pending |
| Release claim | not_releasable |
| Release status | blocked |
| Remaining risk | authenticated_route_fixture_status=fixture_authored;sblr_round_trip_fixture_status=fixture_authored |

## Semantic Oracle

| Field | Value |
| --- | --- |
| Fixture ID | SBSQL-SURFACE-7BC1DB469041 |
| Oracle type | canonical_spec_plus_sblr_matrix |
| Oracle search key | SBSQL-B7DCE9CB07B6 |
| Expected result summary | expected parse, bind, lower, result shape, diagnostic, and executable/refusal behavior derived from canonical spec and operation matrix |
| Oracle closure status | closed_by_semantic_oracle_authority_gate |

## Boundary

- This snapshot is derived only from tracked public release artifacts.
- SQL text remains parser-side input; engine behavior is reached through the published SBLR/internal-API contract.
- This snapshot carries no implementation, source-tree, absolute, or private canonicalization path.
