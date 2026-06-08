# Cluster-Gated Statements

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `syntax_reference_cluster_gated`


## Purpose

Cluster statements are grammar-recognized so clients receive stable diagnostics. Public builds without admitted cluster support return unsupported or unlicensed message vectors. Builds that admit cluster routing call the cluster provider boundary.

Non-cluster bridge queries to a remote table are not cluster distributed queries. A cluster distributed query lets a cluster optimizer coordinate across nodes; that surface is cluster-classified.

Example:

```sql
show cluster status;
```

## Syntax Productions

```ebnf
private_cluster_statement ::= show_cluster | alter_cluster | create_cluster | drop_cluster ;
```

```ebnf
show_cluster            ::= "SHOW" "CLUSTER" cluster_target ;
```

```ebnf
alter_cluster           ::= "ALTER" "CLUSTER" cluster_action ;
```

```ebnf
create_cluster          ::= "CREATE" "CLUSTER" cluster_create_payload ;
```

```ebnf
drop_cluster            ::= "DROP" "CLUSTER" cluster_ref ;
```

## Binding And Execution

- The parser recognizes the syntax and builds a statement or expression tree.
- Binding resolves catalog names, UUID references, parameter descriptors, result descriptors, security context, transaction context, and profile options.
- SBLR admission maps the bound request to an operation family and result shape.
- The engine rechecks authority before durable state changes or result delivery.

## Related Surface Rows

| Surface | Kind | Family | Lowering | Result Shape |
| --- | --- | --- | --- | --- |
| member_ref_list | grammar_production | cluster_private | yes | rs.sbsql.cluster_private_refusal.v1 |
| cluster_setting_stmt | grammar_production | cluster_private | yes | rs.sbsql.cluster_private_refusal.v1 |
| cluster_ref | grammar_production | cluster_private | yes | rs.sbsql.cluster_private_refusal.v1 |
| placement_clause | grammar_production | cluster_private | yes | rs.sbsql.cluster_private_refusal.v1 |
| cluster_prepare_options | grammar_production | cluster_private | yes | rs.sbsql.cluster_private_refusal.v1 |
| cluster_reconcile_stmt | grammar_production | cluster_private | yes | rs.sbsql.cluster_private_refusal.v1 |
| cluster_audit_stmt | grammar_production | cluster_private | yes | rs.sbsql.cluster_private_refusal.v1 |
| cluster_stmt | grammar_production | cluster_private | yes | rs.sbsql.cluster_private_refusal.v1 |
| cluster_tx_stmt | grammar_production | cluster_private | yes | rs.sbsql.cluster_private_refusal.v1 |
| cluster_topology_stmt | grammar_production | cluster_private | yes | rs.sbsql.cluster_private_refusal.v1 |
| region_split_stmt | grammar_production | cluster_private | yes | rs.sbsql.cluster_private_refusal.v1 |
| cluster_lifecycle_ddl | grammar_production | cluster_private | yes | rs.sbsql.cluster_private_refusal.v1 |
| cluster_node_op_stmt | grammar_production | cluster_private | yes | rs.sbsql.cluster_private_refusal.v1 |
| cluster_throttle_stmt | grammar_production | cluster_private | yes | rs.sbsql.cluster_private_refusal.v1 |
| shard_clause | grammar_production | cluster_private | yes | rs.sbsql.cluster_private_refusal.v1 |
| shard_method | grammar_production | cluster_private | yes | rs.sbsql.cluster_private_refusal.v1 |
| cluster_job_control_stmt | grammar_production | cluster_private | yes | rs.sbsql.cluster_private_refusal.v1 |
| cluster_system_op_stmt | grammar_production | cluster_private | yes | rs.sbsql.cluster_private_refusal.v1 |
| cluster_member_op_stmt | grammar_production | cluster_private | yes | rs.sbsql.cluster_private_refusal.v1 |
| cluster_failover_stmt | grammar_production | cluster_private | yes | rs.sbsql.cluster_private_refusal.v1 |
| region_name | grammar_production | cluster_private | yes | rs.sbsql.cluster_private_refusal.v1 |
| cluster_control_stmt | grammar_production | cluster_private | yes | rs.sbsql.cluster_private_refusal.v1 |
| route_ref | grammar_production | cluster_private | yes | rs.sbsql.cluster_private_refusal.v1 |
| member_ref | grammar_production | cluster_private | yes | rs.sbsql.cluster_private_refusal.v1 |
