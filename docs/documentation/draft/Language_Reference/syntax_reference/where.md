# WHERE And Predicates

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `syntax_reference_where`


## Purpose

`WHERE` filters rows using a descriptor-bound predicate. Null and missing semantics come from the bound operation, not from raw textual spelling.

The optimizer may use predicate evidence for an index plan, but the executor must apply final visibility and predicate checks.

Example:

```sql
select *
from app.orders
where order_state = 'open' and submitted_at >= :start_at;
```

## Syntax Productions

```ebnf
where_clause            ::= "WHERE" predicate ;
```

```ebnf
predicate               ::= expression ;
```

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
| query_term | grammar_production | query | yes | rs.sbsql.rowset.v1 |
| ch_join_strictness | grammar_production | query | yes | rs.sbsql.rowset.v1 |
| fulltext_search_query | grammar_production | query | yes | rs.sbsql.rowset.v1 |
| select | canonical_surface | query | yes | rs.sbsql.rowset.v1 |
| select_item | grammar_production | query | yes | rs.sbsql.rowset.v1 |
| clustering_order_spec | grammar_production | query | yes | rs.sbsql.cluster_private_refusal.v1 |
| within_group_clause | grammar_production | query | yes | rs.sbsql.rowset.v1 |
| stream_consumer_group_stmt | grammar_production | query | yes | rs.sbsql.rowset.v1 |
| xml_query_arg | grammar_production | query | yes | rs.sbsql.rowset.v1 |
| limit_offset_clause | grammar_production | query | yes | rs.sbsql.rowset.v1 |
| select_list | grammar_production | query | yes | rs.sbsql.rowset.v1 |
| subquery_expression | grammar_production | query | yes | rs.sbsql.rowset.v1 |
| quota_limit | grammar_production | query | yes | rs.sbsql.rowset.v1 |
| graph_subquery_stmt | grammar_production | query | yes | rs.sbsql.rowset.v1 |
| with_clause | grammar_production | query | yes | rs.sbsql.rowset.v1 |
| cypher_with_clause | grammar_production | query | yes | rs.sbsql.rowset.v1 |
| group_by_list | grammar_production | query | yes | rs.sbsql.rowset.v1 |
| identifier_delimited | grammar_production | query | yes | rs.sbsql.rowset.v1 |
| json_query_form | grammar_production | query | yes | rs.sbsql.rowset.v1 |
| psql_select_into | grammar_production | query | yes | rs.sbsql.rowset.v1 |
| prewhere_clause | grammar_production | query | yes | rs.sbsql.rowset.v1 |
| query_specification | grammar_production | query | yes | rs.sbsql.rowset.v1 |
| vector_search_query | grammar_production | query | yes | rs.sbsql.rowset.v1 |
| xml_query_form | grammar_production | query | yes | rs.sbsql.rowset.v1 |
