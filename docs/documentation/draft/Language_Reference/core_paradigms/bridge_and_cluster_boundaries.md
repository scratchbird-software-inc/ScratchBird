# Bridge And Cluster Boundaries

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `core_paradigms_bridge_and_cluster_boundaries`


## Purpose

Bridge commands connect a local user or agent session to another database endpoint through a registered bridge-capable UDR package. The bridge is a connection boundary. It does not move transaction finality, catalog identity, or storage authority out of the participating databases.

A normal remote-table query is not a distributed query. It treats the remote relation as a local statement input reached through a connection. A distributed or cross-node optimizer decision is cluster-classified and is routed through compile-time cluster gates.

Public cluster surfaces parse far enough to return the correct unsupported or unlicensed message vector unless the build admits routing to the cluster provider boundary.

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
| bridge_stream_open_physical_page_copy_denied | grammar_production | bridge | yes | rs.sbsql.bridge_operation.v1 |
| bridge_begin_transaction | grammar_production | bridge | yes | rs.sbsql.bridge_operation.v1 |
| bridge_stream_open_logical_restore | grammar_production | bridge | yes | rs.sbsql.bridge_operation.v1 |
| bridge_validate_connection | grammar_production | bridge | yes | rs.sbsql.bridge_operation.v1 |
| bridge_stream_read | grammar_production | bridge | yes | rs.sbsql.bridge_operation.v1 |
| bridge_cdc_read | grammar_production | bridge | yes | rs.sbsql.bridge_operation.v1 |
| bridge_detach | grammar_production | bridge | yes | rs.sbsql.bridge_operation.v1 |
| bridge_commit_transaction | grammar_production | bridge | yes | rs.sbsql.bridge_operation.v1 |
| bridge_stream_write | grammar_production | bridge | yes | rs.sbsql.bridge_operation.v1 |
| bridge_ping | grammar_production | bridge | yes | rs.sbsql.bridge_operation.v1 |
| bridge_stream_close | grammar_production | bridge | yes | rs.sbsql.bridge_operation.v1 |
| bridge_authenticate | grammar_production | bridge | yes | rs.sbsql.bridge_operation.v1 |
| bridge_create_connection | grammar_production | bridge | yes | rs.sbsql.bridge_operation.v1 |
| bridge_cdc_start | grammar_production | bridge | yes | rs.sbsql.bridge_operation.v1 |
| bridge_cutover | grammar_production | bridge | yes | rs.sbsql.bridge_operation.v1 |
| bridge_proxy_route | grammar_production | bridge | yes | rs.sbsql.bridge_operation.v1 |
| bridge_savepoint_transaction | grammar_production | bridge | yes | rs.sbsql.bridge_operation.v1 |
| bridge_attach | grammar_production | bridge | yes | rs.sbsql.bridge_operation.v1 |
| bridge_health | grammar_production | bridge | yes | rs.sbsql.bridge_operation.v1 |
| bridge_rollback_transaction | grammar_production | bridge | yes | rs.sbsql.bridge_operation.v1 |
| bridge_cursor_fetch | grammar_production | bridge | yes | rs.sbsql.bridge_operation.v1 |
| bridge_close_session | grammar_production | bridge | yes | rs.sbsql.bridge_operation.v1 |
| bridge_compare_result | grammar_production | bridge | yes | rs.sbsql.bridge_operation.v1 |
| bridge_open_session | grammar_production | bridge | yes | rs.sbsql.bridge_operation.v1 |
