# Show Cluster EBNF Production

This page is part of the SBsql Language Reference Manual. It documents the grammar production for `SHOW CLUSTER` while preserving the public cluster gate: recognized inspection syntax returns authorized provider/refusal information and must not imply production cluster behavior is present.

Generation task: `ebnf_show_cluster`

Parent reference: [Cluster-Gated Statements](../cluster_gated_statements.md)

## Production

```ebnf
show_cluster ::=
    SHOW CLUSTER cluster_target cluster_option_list? ;

cluster_target ::=
      STATUS
    | PROVIDER
    | TOPOLOGY
    | MEMBERS
    | ROUTES
    | PLACEMENT
    | SHARDS
    | TRANSACTIONS
    | JOBS
    | METRICS
    | SUPPORT
    | QUERY PLAN qualified_name ;

cluster_option_list ::=
    WITH cluster_option ("," cluster_option)* ;
```

## Meaning

`show_cluster` recognizes cluster inspection requests. In a public build, the only expected successful information may be provider/refusal metadata such as compile/link-only status. Other targets are cluster-gated and must return unsupported, unlicensed, or fail-closed diagnostics unless an admitted provider boundary exists.

## Binding Requirements

| Element | Binding requirement |
| --- | --- |
| Target | Recognized cluster inspection target. |
| Qualified name | Visible query, route, job, or provider descriptor where the target requires it. |
| Options | Diagnostic, timeout, provider, or dry-run options where admitted. |
| Result | Redacted report shape visible to the caller. |

## Used By

| Parent production | Purpose |
| --- | --- |
| `cluster_stmt` | Places inspection in the cluster-gated statement family. |

## Admission Notes

- `SHOW CLUSTER PROVIDER` can report compile/link-only public stub status.
- `SHOW CLUSTER STATUS` must not invent cluster state from local single-node state.
- Hidden provider, route, member, query, or job details must be redacted or refused.
- Inspection is not mutation and is not transaction finality authority.
