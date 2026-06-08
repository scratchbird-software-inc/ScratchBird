# Scripts, Tokens, And Identifiers

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `syntax_reference_script_and_tokens`


## Purpose

An SBsql script is a sequence of statements separated by terminators. Words are context-sensitive. Most command vocabulary can still be used as identifiers where the grammar expects an identifier.

Object references use either qualified names or UUID references. Qualified names go through the resolver; UUID references are direct identity evidence that must match the requested object class.

Schema variables, search-path behavior, recursive schema branches, identifier-profile folding, and name-resolution order are documented in [schema_tree_and_name_resolution.md](schema_tree_and_name_resolution.md).

Example:

```sql
create schema app;
comment on schema app is 'application schema';
```

## Syntax Productions

```ebnf
script                  ::= statement_list EOF ;
```

```ebnf
statement_list          ::= statement (statement_terminator statement)* statement_terminator? ;
```

```ebnf
statement               ::= native_statement | refusal_statement ;
```

```ebnf
statement_terminator    ::= ";" ;
```

```ebnf
qualified_name          ::= name_part ("." name_part)* ;
```

```ebnf
name_part               ::= regular_identifier | delimited_identifier | localized_name_literal ;
```

```ebnf
uuid_ref                ::= "UUID" string_literal ;
```

```ebnf
option_list             ::= option ("," option)* ;
```

## Binding And Execution

- The parser recognizes the syntax and builds a statement or expression tree.
- Binding resolves catalog names, UUID references, parameter descriptors, result descriptors, security context, transaction context, and profile options.
- SBLR admission maps the bound request to an operation family and result shape.
- The engine rechecks authority before durable state changes or result delivery.

## Related Surface Rows

| Surface | Kind | Family | Lowering | Result Shape |
| --- | --- | --- | --- | --- |
| validator_clause | grammar_production | general | yes | rs.sbsql.structural_lowering.v1 |
| refusal_stmt | grammar_production | general | yes | rs.sbsql.structural_lowering.v1 |
| on_conflict_clause | grammar_production | general | yes | rs.sbsql.structural_lowering.v1 |
| parameter_marker | grammar_production | general | yes | rs.sbsql.structural_lowering.v1 |
| simple_case_expr | grammar_production | general | yes | rs.sbsql.structural_lowering.v1 |
| colocation_clause | grammar_production | general | yes | rs.sbsql.structural_lowering.v1 |
| pivot_clause | grammar_production | general | yes | rs.sbsql.structural_lowering.v1 |
| event_trigger_filter | grammar_production | general | yes | rs.sbsql.structural_lowering.v1 |
| evidence_item | grammar_production | general | yes | rs.sbsql.cluster_private_refusal.v1 |
| psql_repeat_stmt | grammar_production | general | yes | rs.sbsql.structural_lowering.v1 |
| forall_dml_or_execute | grammar_production | general | yes | rs.sbsql.structural_lowering.v1 |
| locality_clause | grammar_production | general | yes | rs.sbsql.structural_lowering.v1 |
| psql_for_stmt | grammar_production | general | yes | rs.sbsql.structural_lowering.v1 |
| aggregate_attr | grammar_production | general | yes | rs.sbsql.structural_lowering.v1 |
| real128_literal | grammar_production | general | yes | rs.sbsql.structural_lowering.v1 |
| xml_forest_element | grammar_production | general | yes | rs.sbsql.structural_lowering.v1 |
| pipeline_clause | grammar_production | general | yes | rs.sbsql.structural_lowering.v1 |
| pivot_agg_list | grammar_production | general | yes | rs.sbsql.structural_lowering.v1 |
| op_class_ref | grammar_production | general | yes | rs.sbsql.structural_lowering.v1 |
| date_literal | grammar_production | general | yes | rs.sbsql.structural_lowering.v1 |
| row_constructor | grammar_production | general | yes | rs.sbsql.structural_lowering.v1 |
| acceleration_stmt | grammar_production | general | yes | rs.sbsql.structural_lowering.v1 |
| keyvalue_op_stmt | grammar_production | general | yes | rs.sbsql.structural_lowering.v1 |
| principal_attribute | grammar_production | general | yes | rs.sbsql.structural_lowering.v1 |
