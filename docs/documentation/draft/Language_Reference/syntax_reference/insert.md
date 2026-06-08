# INSERT Statement

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `syntax_reference_insert`


## Purpose

`INSERT` creates row versions inside the active transaction. Defaults, generated values, domain checks, constraints, index maintenance, and authorization are engine-verified before durable visibility.

`RETURNING` may expose inserted values after defaults and generated columns are applied.

Example:

```sql
insert into app.customer (customer_id, customer_name)
values (:id, :name)
returning customer_id;
```

## Syntax Productions

```ebnf
insert_statement        ::= "INSERT" "INTO" table_ref column_list? values_source returning_clause? ;
```

```ebnf
values_source           ::= "VALUES" row_value_list | query_statement ;
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
