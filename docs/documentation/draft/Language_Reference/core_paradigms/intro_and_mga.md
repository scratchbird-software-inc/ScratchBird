# Intro And MGA Authority

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `core_paradigms_intro_and_mga`


## Purpose

SBsql is a parser language for asking ScratchBird to perform work. The engine does not execute SQL text. The parser lowers admitted text into SBLR envelopes that carry UUID-resolved object identity, descriptor-bound values, transaction context, and diagnostic routing information.

MGA is the transaction authority. A statement may request begin, commit, rollback, savepoint, retain, chain, or remote bridge participation, but finality belongs to the database transaction inventory and recovery model. This keeps SBsql syntax, client tools, and parser packages from becoming storage or recovery authority.

A practical reading rule follows from this: treat every name, keyword, and option in SBsql as evidence that must be bound. Treat every durable object, transaction outcome, and security decision as engine-owned state.

## Syntax Productions

```ebnf
script                  ::= statement_list EOF ;
```

```ebnf
statement               ::= native_statement | refusal_statement ;
```

```ebnf
transaction_statement   ::= begin_transaction | commit_transaction | rollback_transaction | savepoint_statement | set_transaction | show_transaction ;
```

## Binding And Execution

- The parser recognizes the syntax and builds a statement or expression tree.
- Binding resolves catalog names, UUID references, parameter descriptors, result descriptors, security context, transaction context, and SBsql execution options.
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
