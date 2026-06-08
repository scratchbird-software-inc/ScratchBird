# DELETE Statement

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `syntax_reference_delete`


## Purpose

`DELETE` retires row visibility by transaction state. Cleanup and physical reclamation are separate engine decisions.

A deleted row can still be visible to older valid snapshots until MGA cleanup policy allows reclamation.

Example:

```sql
delete from app.session_token
where expires_at < current_timestamp
returning token_id;
```

## Syntax Productions

```ebnf
delete_statement        ::= "DELETE" "FROM" table_ref where_clause? returning_clause? ;
```

```ebnf
where_clause            ::= "WHERE" predicate ;
```

```ebnf
returning_clause        ::= "RETURNING" projection_list ;
```

## Binding And Execution

- The parser recognizes the syntax and builds a statement or expression tree.
- Binding resolves catalog names, UUID references, parameter descriptors, result descriptors, security context, transaction context, and profile options.
- SBLR admission maps the bound request to an operation family and result shape.
- The engine rechecks authority before durable state changes or result delivery.

## Related Surface Rows

| Surface | Kind | Family | Lowering | Result Shape |
| --- | --- | --- | --- | --- |
| cypher_delete_clause | grammar_production | dml | yes | rs.sbsql.command_completion.v1 |
| upsert | canonical_surface | dml | yes | rs.sbsql.command_completion.v1 |
| graph_delete_node_stmt | grammar_production | dml | yes | rs.sbsql.command_completion.v1 |
| cypher_merge_action | grammar_production | dml | yes | rs.sbsql.command_completion.v1 |
| delete | canonical_surface | dml | yes | rs.sbsql.command_completion.v1 |
| copy_format | grammar_production | dml | yes | rs.sbsql.command_completion.v1 |
| merge_strategy | grammar_production | dml | yes | rs.sbsql.command_completion.v1 |
| bulk_target_list | grammar_production | dml | yes | rs.sbsql.command_completion.v1 |
| graph_delete_edge_stmt | grammar_production | dml | yes | rs.sbsql.command_completion.v1 |
| copy_options | grammar_production | dml | yes | rs.sbsql.command_completion.v1 |
| copy_import_export | canonical_surface | dml | yes | rs.sbsql.command_completion.v1 |
| doc_bulk_op | grammar_production | dml | yes | rs.sbsql.command_completion.v1 |
| copy_statement | grammar_production | dml | yes | rs.sbsql.command_completion.v1 |
| doc_bulk_stmt | grammar_production | dml | yes | rs.sbsql.command_completion.v1 |
| update_statement | grammar_production | dml | yes | rs.sbsql.command_completion.v1 |
| merge | canonical_surface | dml | yes | rs.sbsql.command_completion.v1 |
| gpu_workload_action | grammar_production | dml | yes | rs.sbsql.command_completion.v1 |
| lock_row_for_update | canonical_surface | dml | yes | rs.sbsql.command_completion.v1 |
| upsert_statement | grammar_production | dml | yes | rs.sbsql.command_completion.v1 |
| insert_statement | grammar_production | dml | yes | rs.sbsql.command_completion.v1 |
| merge_when_clause | grammar_production | dml | yes | rs.sbsql.command_completion.v1 |
| doc_update_op | grammar_production | dml | yes | rs.sbsql.command_completion.v1 |
| doc_update_verb | grammar_production | dml | yes | rs.sbsql.command_completion.v1 |
| cypher_load_csv | grammar_production | dml | yes | rs.sbsql.command_completion.v1 |
