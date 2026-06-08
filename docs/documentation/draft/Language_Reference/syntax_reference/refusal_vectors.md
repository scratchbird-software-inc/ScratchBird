# Refusal Vectors

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `syntax_reference_refusal_vectors`


## Purpose

A recognized command may still be refused. The three high-level refusal classes are unsupported, denied, and unlicensed.

Unsupported means the surface is not available in the SBsql or build. Denied means authorization, sandboxing, policy, or safety rules refuse it. Unlicensed means the parser and route are known but the capability is not licensed for the running product profile.

Example:

```sql
-- The exact text is parsed to its surface and returns a message vector when gated.
show cluster status;
```

## Syntax Productions

```ebnf
refusal_statement       ::= unsupported_statement | denied_statement | unlicensed_statement ;
```

```ebnf
unsupported_statement   ::= unsupported_token_sequence ;
```

```ebnf
denied_statement        ::= denied_token_sequence ;
```

```ebnf
unlicensed_statement    ::= unlicensed_token_sequence ;
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
