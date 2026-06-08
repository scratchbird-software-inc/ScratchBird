# SELECT Statement

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `syntax_reference_select`


## Purpose

`SELECT` reads rows through relation descriptors and MGA visibility. Projection expressions are descriptor-bound, predicates are bound before execution, and result ordering is deterministic only when an admitted ordering surface supplies it.

The optimizer may use indexes, statistics, and expression metadata, but candidate indexes never become final row authority. The executor must revalidate row visibility and predicate truth.

Example:

```sql
select customer_id, count(*) as order_count
from app.orders
where order_state = 'open'
group by customer_id
having count(*) > 0
order by customer_id
limit 100;
```

## Syntax Productions

```ebnf
query_statement         ::= select_statement | with_statement | values_statement | nosql_query_statement ;
```

```ebnf
select_statement        ::= "SELECT" select_modifier? projection_list from_clause? where_clause? group_by_clause? having_clause? window_clause? order_by_clause? limit_clause? ;
```

```ebnf
select_modifier         ::= "ALL" | "DISTINCT" ;
```

```ebnf
projection_list         ::= projection ("," projection)* ;
```

```ebnf
from_clause             ::= "FROM" table_expression ;
```

```ebnf
where_clause            ::= "WHERE" predicate ;
```

```ebnf
group_by_clause         ::= "GROUP" "BY" expression_list ;
```

```ebnf
having_clause           ::= "HAVING" predicate ;
```

```ebnf
window_clause           ::= "WINDOW" window_definition_list ;
```

```ebnf
order_by_clause         ::= "ORDER" "BY" ordering_list ;
```

```ebnf
limit_clause            ::= "LIMIT" expression ("OFFSET" expression)? ;
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
