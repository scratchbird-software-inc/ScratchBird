# ORDER BY, LIMIT, And OFFSET

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `syntax_reference_order_limit`


## Purpose

`ORDER BY` defines client-visible ordering. Without it, row order is not a contract even if a plan happens to read an index.

`LIMIT` and `OFFSET` restrict delivered rows. They should be paired with an order when a script expects stable pagination.

Example:

```sql
select *
from app.orders
order by submitted_at desc, order_id
limit 50 offset 100;
```

## Syntax Productions

```ebnf
order_by_clause         ::= "ORDER" "BY" ordering_list ;
```

```ebnf
limit_clause            ::= "LIMIT" expression ("OFFSET" expression)? ;
```

## Binding And Execution

- The parser recognizes the syntax and builds a statement or expression tree.
- Binding resolves catalog names, UUID references, parameter descriptors, result descriptors, security context, transaction context, and SBsql execution options.
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
