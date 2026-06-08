# Parser To SBLR Pipeline

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `core_paradigms_parser_to_sblr`


## Purpose

The parser pipeline starts with contextual tokenization, builds a lossless parse tree, binds names to UUID catalog identity, attaches descriptor information, and emits an SBLR operation family. The engine verifier then validates the envelope before execution.

This separation is deliberate. It lets SBsql stay expressive and context sensitive while the engine keeps one authoritative binary command language. Parser packages for donor dialects follow the same rule: they translate donor text into ScratchBird authority, not into donor engine authority.

A successful statement therefore has three visible phases: parse success, bind/admission success, and engine execution success. A failure in any phase returns a message vector rather than continuing with guessed behavior.

## Syntax Productions

```ebnf
script                  ::= statement_list EOF ;
```

```ebnf
statement               ::= native_statement | refusal_statement ;
```

```ebnf
native_statement        ::= query_statement
                          | dml_statement
                          | ddl_statement
                          | transaction_statement
                          | security_statement
                          | policy_statement
                          | observability_statement
                          | management_statement
                          | acceleration_statement
                          | archive_replication_migration_statement
                          | nosql_statement
                          | private_cluster_statement ;
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
