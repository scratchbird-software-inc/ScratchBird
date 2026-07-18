# SBSQL-4976BE206EC9 — st_distance

Generated public per-element contract snapshot.

## Identity

| Field | Value |
| --- | --- |
| Surface ID | SBSQL-4976BE206EC9 |
| Fixed UUID v7 | 019dffbb-f000-70cc-960f-177f0b06f890 |
| Canonical name | st_distance |
| Surface kind | function |
| Family | expression_runtime |

## Route Contract

| Field | Value |
| --- | --- |
| Source status | native_now |
| Cluster scope | noncluster_or_profile_scoped |
| SBLR operation family | sblr.expression.runtime.v3 |
| Diagnostic target | canonical_message_vector_and_parser_rendering |
| Final acceptance rule | implement_full_route_or_exact_canonical_refusal |
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
| Fixture ID | SBSQL-SURFACE-100AA9C08DE0 |
| Oracle type | canonical_spec_plus_sblr_matrix |
| Oracle search key | SBSQL-4976BE206EC9 |
| Expected result summary | expected parse, bind, lower, result shape, diagnostic, and executable/refusal behavior derived from canonical spec and operation matrix |
| Oracle closure status | closed_by_semantic_oracle_authority_gate |

## Boundary

- This snapshot is derived only from tracked public release artifacts.
- SQL text remains parser-side input; engine behavior is reached through the published SBLR/internal-API contract.
- This snapshot carries no implementation, source-tree, absolute, or private canonicalization path.
