# WITH And Common Table Expressions

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `syntax_reference_with`


## Purpose

`WITH` introduces named common table expressions for the following query. CTE names are statement-local resolver entries, not durable catalog objects. `RECURSIVE` is a contextual modifier for CTE binding, not a globally reserved word in SBsql.

A CTE can make complex queries readable, isolate reusable subqueries, and express staged transformations. Recursive behavior, materialization, and optimization choices must follow the admitted profile and result-shape contract.

Example:

```sql
with open_orders as (
  select * from app.orders where order_state = 'open'
)
select customer_id, count(*)
from open_orders
group by customer_id;
```

## Syntax Productions

```ebnf
with_statement          ::= "WITH" "RECURSIVE"? cte_list select_statement ;
```

```ebnf
cte_list                ::= cte ("," cte)* ;
```

```ebnf
cte                     ::= identifier column_alias_list? "AS" "(" query_statement ")" ;
```

```ebnf
recursive_values_cte    ::= "WITH" "RECURSIVE" identifier column_alias_list?
                            "AS" "(" values_source "UNION" "DISTINCT"? values_source ")"
                            select_statement ;
```

## Recursive CTE Route

SBsql exposes recursive CTEs through SBLR query-plan operation `SBLR_QUERY_PLAN_OPERATION` with the `values_recursive_cte` payload family. The admitted route is a bounded fixed-point route for values-backed anchor and recursive members:

```sql
with recursive c(n) as (
  values (1)
  union
  values (2), (3)
)
select * from c;
```

This route materializes the anchor relation, applies the recursive member as an additional relation, and returns the fixed-point rowset under the `recursive_fixed_point_materialized` strategy. It preserves the normal parser/engine split: the parser may describe the CTE shape, but the engine owns execution admission, transaction context, result production, and diagnostics.

Current admission rules are deliberately strict:

- The supported route uses `UNION` or `UNION DISTINCT`; `UNION ALL` is refused because duplicate-preserving recursion requires a separate execution contract.
- Column aliases, when supplied, must match the CTE row arity and become the output descriptor names.
- The outer query for this exact route must select from the declared CTE.
- Table-backed recursive references, search/cycle clauses, and recursive terms that require expression evaluation over the previous iteration must map to a future admitted route or fail closed with a rendered unsupported diagnostic.
- SBsql parsers for engines that support recursive CTEs must lower only shapes that the SBsql/SBLR route admits; they must not silently approximate recursive semantics.

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
