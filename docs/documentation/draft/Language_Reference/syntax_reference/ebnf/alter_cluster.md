# Alter Cluster EBNF Production

This page is part of the SBsql Language Reference Manual. It documents the grammar production for `ALTER CLUSTER` while preserving the public cluster gate: recognized topology, routing, placement, transaction, security, job, and validation syntax returns stable diagnostics unless an admitted provider boundary exists.

Generation task: `ebnf_alter_cluster`

Parent reference: [Cluster-Gated Statements](../cluster_gated_statements.md)

## Production

```ebnf
alter_cluster ::=
    ALTER CLUSTER cluster_ref cluster_action cluster_option_list? ;

cluster_action ::=
      SET cluster_setting_list
    | ADD MEMBER member_ref
    | DROP MEMBER member_ref
    | DRAIN MEMBER member_ref
    | SET MEMBER member_ref ROLE cluster_member_role
    | DEFINE REGION region_name
    | DEFINE SHARD PROFILE shard_profile_ref
    | PUBLISH ROUTE route_ref
    | REBALANCE placement_clause
    | START JOB cluster_job_ref
    | CANCEL JOB cluster_job_ref
    | THROTTLE cluster_throttle_payload
    | VALIDATE cluster_validation_target
    | RECONCILE cluster_reconcile_target
    | FAILOVER cluster_failover_target ;

cluster_option_list ::=
    WITH cluster_option ("," cluster_option)* ;
```

## Meaning

`alter_cluster` recognizes cluster mutation, control, validation, and administrative syntax. The public parser can classify these operations, but public builds must not execute production cluster behavior through local core code.

## Binding Requirements

| Element | Binding requirement |
| --- | --- |
| Cluster reference | Visible cluster resolver input where provider admission exists. |
| Action | Normalized cluster operation family. |
| Action payload | Member, route, placement, region, job, validation, reconciliation, failover, or setting descriptors. |
| Options | Provider, manifest, digest, dry-run, validate-only, timeout, and diagnostic options. |

## Used By

| Parent production | Purpose |
| --- | --- |
| `private_cluster_statement` | Places cluster mutation/control in the cluster-gated statement family. |

## Admission Notes

- Public builds return unsupported, unlicensed, or fail-closed diagnostics.
- Local single-node maintenance cannot become cluster maintenance through this syntax.
- Distributed transaction and query actions require provider admission.
- Local MGA remains local finality authority even when a provider route is admitted.
