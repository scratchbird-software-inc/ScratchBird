# SBSQL-672A93B5E0EC — bridge_stream_close

Generated public per-element contract snapshot.

## Identity

| Field | Value |
| --- | --- |
| Surface ID | SBSQL-672A93B5E0EC |
| Fixed UUID v7 | 019e14c1-1984-73f0-8097-4fc74c538744 |
| Canonical name | bridge_stream_close |
| Surface kind | grammar_production |
| Family | bridge |

## Route Contract

| Field | Value |
| --- | --- |
| Source status | native_now |
| Cluster scope | noncluster_or_profile_scoped |
| SBLR operation family | sblr.bridge.operation.v3 |
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
| Fixture ID | SBSQL-SURFACE-672A93B5E0EC |
| Oracle type | canonical_spec_plus_sblr_matrix |
| Oracle search key | SBSQL-672A93B5E0EC |
| Expected result summary | expected parser bridge command route, SBLR bridge operation envelope, opcode SBLR_BRIDGE_STREAM_CLOSE, UDR operation stream_close, MGA-preserving local and remote transaction authority, and reference-specific rendering derived from the universal bridge ABI |
| Oracle closure status | closed_by_semantic_oracle_authority_gate |

## Boundary

- This snapshot is derived only from tracked public release artifacts.
- SQL text remains parser-side input; engine behavior is reached through the published SBLR/internal-API contract.
- This snapshot carries no implementation, source-tree, absolute, or private canonicalization path.
