# Domains, Casts, And Coercion

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `data_types_domains_and_casts`


## Purpose

Domains are catalog objects that wrap descriptors with policy. A domain can define nullability, defaults, constraints, element policy, masking, donor-facing type names, and cast behavior. A cast does not erase domain policy unless the cast policy says it may.

Implicit coercion is descriptor-driven and conservative. Explicit casts request a target descriptor or domain; the binder then applies lossiness, security, and compatibility rules.

Example:

```sql
create domain app.positive_amount as decimal(18, 2)
  check (value >= 0);
select cast(:amount as app.positive_amount);
```

## Syntax Productions

```ebnf
expression              ::= expression_atom (binary_operator expression_atom)* ;
```

## Binding And Execution

- The parser recognizes the syntax and builds a statement or expression tree.
- Binding resolves catalog names, UUID references, parameter descriptors, result descriptors, security context, transaction context, and profile options.
- SBLR admission maps the bound request to an operation family and result shape.
- The engine rechecks authority before durable state changes or result delivery.

## Related Surface Rows

| Surface | Kind | Family | Lowering | Result Shape |
| --- | --- | --- | --- | --- |
| domain_method_block | grammar_production | ddl_catalog | yes | rs.sbsql.command_completion.v1 |
| create_materialized_view_stmt | grammar_production | ddl_catalog | yes | rs.sbsql.command_completion.v1 |
| data_type_list | grammar_production | ddl_catalog | yes | rs.sbsql.command_completion.v1 |
| document_schema_ref | grammar_production | ddl_catalog | yes | rs.sbsql.command_completion.v1 |
| create_index_template_stmt | grammar_production | ddl_catalog | yes | rs.sbsql.command_completion.v1 |
| temporal_type | grammar_production | ddl_catalog | yes | rs.sbsql.command_completion.v1 |
| reindex_vector_options | grammar_production | ddl_catalog | yes | rs.sbsql.command_completion.v1 |
| create_cast_stmt | grammar_production | ddl_catalog | yes | rs.sbsql.command_completion.v1 |
| create_server_stmt | grammar_production | ddl_catalog | yes | rs.sbsql.command_completion.v1 |
| table_name | grammar_production | ddl_catalog | yes | rs.sbsql.command_completion.v1 |
| create_procedure_stmt | grammar_production | ddl_catalog | yes | rs.sbsql.command_completion.v1 |
| comment_doc | grammar_production | ddl_catalog | yes | rs.sbsql.command_completion.v1 |
| index_options | grammar_production | ddl_catalog | yes | rs.sbsql.command_completion.v1 |
| xml_table_form | grammar_production | ddl_catalog | yes | rs.sbsql.command_completion.v1 |
| numeric_type | grammar_production | ddl_catalog | yes | rs.sbsql.command_completion.v1 |
| show_storage_buffer_io_index | grammar_production | ddl_catalog | yes | rs.sbsql.command_completion.v1 |
| cursor_type | grammar_production | ddl_catalog | yes | rs.sbsql.command_completion.v1 |
| drop_filespace_stmt | grammar_production | ddl_catalog | yes | rs.sbsql.command_completion.v1 |
| alter_ts_action | grammar_production | ddl_catalog | yes | rs.sbsql.command_completion.v1 |
| alter_database_action | grammar_production | ddl_catalog | yes | rs.sbsql.command_completion.v1 |
| index_element | grammar_production | ddl_catalog | yes | rs.sbsql.command_completion.v1 |
| create_key_value_store | grammar_production | ddl_catalog | yes | rs.sbsql.command_completion.v1 |
| multiset_type | grammar_production | ddl_catalog | yes | rs.sbsql.command_completion.v1 |
| index_target | grammar_production | ddl_catalog | yes | rs.sbsql.command_completion.v1 |
