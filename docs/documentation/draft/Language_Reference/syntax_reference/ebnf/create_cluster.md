# Create Cluster EBNF Production

This page is part of the SBsql Language Reference Manual. It documents the grammar production for `CREATE CLUSTER` while preserving the public cluster gate: recognized lifecycle syntax returns stable diagnostics unless an admitted provider boundary exists.

Generation task: `ebnf_create_cluster`

Parent reference: [Cluster-Gated Statements](../cluster_gated_statements.md)

## Production

```ebnf
create_cluster_stmt ::=
    CREATE CLUSTER cluster_ref cluster_create_payload? cluster_option_list? ;

cluster_create_payload ::=
      cluster_topology_payload
    | cluster_member_payload
    | cluster_route_payload
    | cluster_placement_payload
    | cluster_security_payload ;

cluster_option_list ::=
    WITH cluster_option ("," cluster_option)* ;
```

## Meaning

`create_cluster_stmt` recognizes cluster lifecycle creation syntax. The public parser can classify and diagnose the statement, but public builds do not create production cluster membership, topology, routing, placement, failover, replication, or distributed transaction behavior.

## Binding Requirements

| Element | Binding requirement |
| --- | --- |
| Cluster reference | Visible cluster resolver input where provider admission exists. |
| Payload | Topology, member, route, placement, or security descriptors. |
| Provider option | Provider descriptor and ABI requirements where supplied. |
| Validation options | Dry-run, validate-only, manifest, digest, timeout, and diagnostic options. |

## Used By

| Parent production | Purpose |
| --- | --- |
| `private_cluster_statement` | Places lifecycle creation in the cluster-gated statement family. |

## Admission Notes

- Public builds should parse and lower far enough to return stable refusal diagnostics.
- `VALIDATE ONLY` can validate grammar and visible descriptors but cannot create cluster authority.
- Production execution requires an admitted provider handshake and operation admission.
- Local catalog mutation must not masquerade as cluster creation.
