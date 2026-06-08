# UUID Catalog Identity

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `core_paradigms_uuid_catalog_identity`


## Purpose

ScratchBird catalog identity is UUID based. User-facing names, localized names, aliases, synonyms, and SBsql-visible spellings are resolver inputs. They are not durable identity.

The schema-tree and name-resolution contract is documented in [../syntax_reference/schema_tree_and_name_resolution.md](../syntax_reference/schema_tree_and_name_resolution.md), including current schema, home schema, search path, recursive schemas, and SBsql-parser sandbox roots.

This is why DDL and catalog inspection statements show both name-oriented behavior and UUID-oriented binding. Creating an object assigns stable identity. Renaming an object changes resolver state. Dropping or retiring an object updates catalog lifecycle and dependency state.

When writing SBsql, prefer names for ordinary administration and UUID references for unambiguous automation, migration, and support diagnostics.

## Syntax Productions

```ebnf
object_ref              ::= uuid_ref | qualified_name ;
```

```ebnf
qualified_name          ::= name_part ("." name_part)* ;
```

```ebnf
uuid_ref                ::= "UUID" string_literal ;
```

## Binding And Execution

- The parser recognizes the syntax and builds a statement or expression tree.
- Binding resolves catalog names, UUID references, parameter descriptors, result descriptors, security context, transaction context, and SBsql execution options.
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
