# Drop Cluster EBNF Production

This page is part of the SBsql Language Reference Manual. It documents the grammar production for `DROP CLUSTER` while preserving the public cluster gate: recognized lifecycle removal syntax returns stable diagnostics unless an admitted provider boundary exists.

Generation task: `ebnf_drop_cluster`

Parent reference: [Cluster-Gated Statements](../cluster_gated_statements.md)

## Production

```ebnf
drop_cluster_stmt ::=
    DROP CLUSTER cluster_ref drop_cluster_option_list? ;

drop_cluster_option_list ::=
    WITH drop_cluster_option ("," drop_cluster_option)* ;

drop_cluster_option ::=
      IF EXISTS
    | RESTRICT
    | CASCADE
    | VALIDATE ONLY ;
```

## Meaning

`drop_cluster_stmt` recognizes cluster lifecycle removal syntax. In public builds, it is a recognized gated surface that returns stable refusal diagnostics. It must not remove local database state, filespaces, routes, or provider metadata as a substitute for production cluster removal.

## Binding Requirements

| Element | Binding requirement |
| --- | --- |
| Cluster reference | Visible cluster resolver input where provider admission exists. |
| Restrict/cascade option | Dependency policy descriptor where admitted. |
| Validate-only option | Diagnostic route that proves what would be refused or admitted. |

## Used By

| Parent production | Purpose |
| --- | --- |
| `private_cluster_statement` | Places lifecycle removal in the cluster-gated statement family. |

## Admission Notes

- `IF EXISTS` may suppress object-not-found diagnostics only where disclosure policy admits it.
- `RESTRICT` and `CASCADE` are provider-admitted lifecycle policies, not local filesystem actions.
- Public builds should return unsupported, unlicensed, or fail-closed diagnostics.
- Local catalog cleanup must not masquerade as cluster removal.
