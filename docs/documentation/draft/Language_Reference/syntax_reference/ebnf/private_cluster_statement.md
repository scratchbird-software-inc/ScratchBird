# Cluster-Gated Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It documents the top-level grammar production for cluster-gated statements. Parsing recognizes shape; binding resolves descriptors and UUID catalog identity; SBLR classifies the operation as cluster-gated; and the provider gate returns unsupported, unlicensed, fail-closed, or admitted-provider behavior.

Generation task: `ebnf_private_cluster_statement`

Parent reference: [Cluster-Gated Statements](../cluster_gated_statements.md)

## Production

```ebnf
private_cluster_statement ::=
      show_cluster
    | alter_cluster
    | create_cluster
    | drop_cluster ;
```

## Meaning

`private_cluster_statement` groups public parser surfaces for cluster-classified operations. The production name is historical grammar terminology. It does not mean cluster implementation code is present in the public build.

After parsing, the binder and SBLR admission layer must classify the operation as cluster-gated. Public builds either return unsupported diagnostics before provider routing or route to the compile/link stub and return fail-closed diagnostics.

## Used By

| Parent production | Purpose |
| --- | --- |
| `native_statement` | Allows recognized cluster statements in ordinary SBsql statement streams. |
| `script_statement` | Allows scripts to receive stable diagnostics for gated cluster surfaces. |

## Child Productions

| Child production | Role |
| --- | --- |
| `show_cluster` | Provider and cluster inspection surfaces. |
| `create_cluster` | Cluster lifecycle creation surfaces. |
| `alter_cluster` | Cluster topology, membership, routing, placement, job, security, and validation surfaces. |
| `drop_cluster` | Cluster lifecycle removal surfaces. |

## Admission Notes

- This production is a gate, not execution authority.
- Local database state must not be presented as cluster state.
- Provider inspection may report compile/link-only status in public builds.
- Production cluster behavior requires an admitted provider boundary.
- Local MGA remains local transaction finality authority.
