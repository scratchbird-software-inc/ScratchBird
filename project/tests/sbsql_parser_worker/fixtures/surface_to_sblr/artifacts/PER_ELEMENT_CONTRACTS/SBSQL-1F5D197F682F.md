# SBSQL-1F5D197F682F — partition_ref

Generated public per-element contract snapshot.

## Identity

| Field | Value |
| --- | --- |
| Surface ID | SBSQL-1F5D197F682F |
| Fixed UUID v7 | 019dffbb-f000-71b9-8183-a6c4de2150cd |
| Canonical name | partition_ref |
| Surface kind | grammar_production |
| Family | general |

## Route Contract

| Field | Value |
| --- | --- |
| Source status | cluster_private |
| Cluster scope | cluster_private |
| SBLR operation family | sblr.general.operation.v3 |
| Diagnostic target | canonical_message_vector_and_parser_rendering |
| Final acceptance rule | parse_bind_lower_server_engine_diagnostic_and_regression_evidence |
| Closure action | gate_by_cluster_profile_and_fail_closed_in_standalone |

## Release Closure

| Field | Value |
| --- | --- |
| Backlog closure status | exact_refusal_passed |
| Release final status | cluster_provider_route_passed |
| Release claim | cluster_public_fail_closed_provider_gated |
| Release status | row_evidence_complete |
| Remaining risk | none |

## Semantic Oracle

| Field | Value |
| --- | --- |
| Fixture ID | SBSQL-SURFACE-BD98D55B94A5 |
| Oracle type | cluster_profile_and_standalone_refusal_policy |
| Oracle search key | SBSQL-1F5D197F682F |
| Expected result summary | standalone fail-closed diagnostic plus cluster-profile behavior when profile is enabled |
| Oracle closure status | closed_by_semantic_oracle_authority_gate |

## Boundary

- This snapshot is derived only from tracked public release artifacts.
- SQL text remains parser-side input; engine behavior is reached through the published SBLR/internal-API contract.
- This snapshot carries no implementation, source-tree, absolute, or private canonicalization path.
