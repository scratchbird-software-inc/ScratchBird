# SELECT Statement

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `syntax_reference_select`


## Purpose

`SELECT` reads rowsets through descriptor-bound sources. A source may be a relational table, view, catalog projection view, bridge relation, document collection projection, graph query projection, vector-search projection, full-text/search projection, time-series projection, or key-value projection when that source has been admitted as a rowset-producing query.

Projection expressions are descriptor-bound, predicates are bound before execution, and result ordering is deterministic only when an admitted ordering surface supplies it. The optimizer may use indexes, statistics, full-text evidence, vector candidate sets, graph adjacency evidence, document-path indexes, and expression metadata, but candidate evidence never becomes final row authority. The executor must revalidate row visibility, authorization, descriptor compatibility, and predicate truth before returning rows.

Use `SELECT` when the requested result is a tabular rowset. Use the dedicated multimodel statement families described in [Multimodel Statements](multimodel_statements.md) when the operation is primarily a document, graph, vector, search, time-series, or key-value command rather than a relational projection.

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

## Query Source Families

`SELECT` can project from these source families when the binder can resolve a descriptor and SBLR admits the route:

| Source family | Typical source shape | Result rule |
| --- | --- | --- |
| Relational | Tables, views, CTEs, derived tables, catalog projection views | Produces ordinary rows and columns. |
| Document or JSON | Document-typed columns, document collections exposed through rowset descriptors, JSON table/value functions | Missing-field and null behavior follows the bound JSON/document operation. |
| Key-value | Key, value, version, TTL, stream, or map/set projections exposed through rowset descriptors | Keys and values must have explicit descriptors; raw storage keys are not authority. |
| Graph | Graph traversal or pattern results projected as rows | Node, edge, path, and property values are row descriptors, not unmanaged graph handles. |
| Vector | Vector columns or vector-search candidate sets projected as rows | Candidate sets must be rechecked against descriptors, visibility, and metric policy. |
| Full-text/search | Search result projections or search-scored rowsets | Search evidence can rank candidates but cannot bypass row visibility or authorization. |
| Time-series | Time-window, bucket, sample, or event projections | Window boundaries, timestamp types, and ordering must be descriptor-bound. |

Related reference pages:

- [FROM And Table Expressions](from.md)
- [WITH And Common Table Expressions](with.md)
- [Document, Graph, Vector, And Multimodel Types](../data_types/document_graph_vector_and_multimodel_types.md)
- [JSON Functions](../functional_reference/sb_json.md)
- [Vector Functions](../functional_reference/sb_vector.md)

## Relational SELECT

The ordinary relational form reads tables, views, CTEs, derived tables, and catalog projection views.

```sql
select o.order_id,
       o.customer_id,
       o.submitted_at,
       sum(i.extended_price) as order_total
from app.orders o
join app.order_items i on i.order_id = o.order_id
where o.order_state = 'open'
group by o.order_id, o.customer_id, o.submitted_at
order by o.submitted_at, o.order_id;
```

This route binds relation names, column names, aggregate descriptors, predicate descriptors, and result descriptors before execution. Authorization is checked against the resolved catalog objects, not against the text spelling of the names.

## Document And JSON Projection

Document data can participate in `SELECT` when it is stored in descriptor-bound document columns or exposed as a rowset-producing document projection. JSON and document functions let a query project, filter, and group document fields without treating a raw document path as storage authority.

```sql
select d.document_id,
       json_value(d.body, '$.customer.id') as customer_id,
       json_value(d.body, '$.status') as status,
       json_value(d.body, '$.total') as total_text
from app.order_documents d
where json_exists(d.body, '$.customer.id')
  and json_value(d.body, '$.status') = 'open'
order by d.document_id;
```

When a document path is missing, the result follows the semantics of the bound JSON/document operation. A predicate that compares a missing value, JSON null, SQL `NULL`, text, number, or boolean must bind to a known descriptor and coercion rule.

## Key-Value Projection

Key-value data can be queried through a rowset projection when the key, value, version, and optional metadata descriptors are known to the binder.

```sql
select c.cache_key,
       c.value_descriptor,
       c.value_payload,
       c.expires_at
from app.session_cache c
where c.cache_key like 'session:%'
  and c.expires_at > current_timestamp
order by c.cache_key
limit 50;
```

The query reads the key-value projection as a rowset. It does not give the parser or client direct authority over the underlying key-value storage layout.

## Vector Projection And Ranking

Vector columns and vector candidate sets can be projected through `SELECT`. Vector search evidence can narrow or order candidates, but final row delivery still requires descriptor, visibility, and predicate checks.

```sql
select p.product_id,
       p.display_name,
       l2_distance(e.embedding, vector(:query_embedding)) as distance
from app.products p
join app.product_embeddings e on e.product_id = p.product_id
where p.is_active = true
  and vector_dims(e.embedding) = vector_dims(vector(:query_embedding))
order by distance, p.product_id
limit 20;
```

If a vector index is used, the index supplies candidates. It does not decide final result membership by itself.

## Full-Text And Search Projection

Full-text or search-backed rowsets can be joined with relational data when the search surface exposes descriptor-bound result columns such as object identity, rank, score, snippet, or matched fields.

```sql
with hits as (
  select h.object_uuid,
         h.score,
         h.snippet
  from app.product_search_hits h
  where h.query_text = :search_text
)
select p.product_id,
       p.display_name,
       hits.score,
       hits.snippet
from hits
join app.products p on p.object_uuid = hits.object_uuid
where p.is_active = true
order by hits.score desc, p.product_id
limit 25;
```

Search score is ranking evidence. The joined row is still read under normal table visibility and authorization rules.

## Graph Projection

Graph query results can feed a `SELECT` when the graph route returns a rowset descriptor. The rowset may contain node identifiers, edge identifiers, path values, properties, distance, depth, or other graph-derived columns.

```sql
with related_products as (
  graph subquery app.customer_product_graph
)
select p.product_id,
       p.display_name,
       related_products.path_depth
from related_products
join app.products p on p.product_id = related_products.product_id
where related_products.customer_id = :customer_id
order by related_products.path_depth, p.product_id;
```

Graph traversal evidence does not bypass relational authorization. If the query joins graph results to relational tables, each side keeps its own descriptor and authorization checks.

## Time-Series Projection

Time-series sources can be projected through `SELECT` when bucket, sample, timestamp, value, and series identity descriptors are bound.

```sql
select m.series_id,
       m.bucket_start,
       avg(m.value) as avg_value,
       max(m.value) as max_value
from app.metric_samples m
where m.metric_name = 'cpu.user'
  and m.bucket_start >= :start_at
  and m.bucket_start < :end_at
group by m.series_id, m.bucket_start
order by m.series_id, m.bucket_start;
```

Time-zone handling, timestamp precision, gap filling, and interpolation are admitted operation details. The `SELECT` result is still a rowset with typed columns.

## Mixed SQL And Multimodel Queries

Mixed queries are expected when all participating sources are rowset-producing and descriptor-bound. This is the common pattern for joining relational catalog data to document fields, vector ranking, search hits, graph paths, or time-series buckets.

```sql
select p.product_id,
       json_value(p.profile_document, '$.brand') as brand,
       h.score as search_score,
       l2_distance(e.embedding, vector(:query_embedding)) as vector_distance
from app.products p
join app.product_embeddings e on e.product_id = p.product_id
join app.product_search_hits h on h.object_uuid = p.object_uuid
where json_value(p.profile_document, '$.status') = 'active'
  and h.query_text = :search_text
order by h.score desc, vector_distance, p.product_id
limit 20;
```

The engine treats this as one bound query plan with multiple evidence sources. Search and vector evidence can influence candidate order; JSON predicates can filter rows; relation descriptors and MGA visibility decide which rows can actually be returned.

## Dedicated NoSQL Query Surfaces

The query grammar includes rowset-producing NoSQL query surfaces. Dedicated `DOCUMENT`, `GRAPH`, `VECTOR`, `SEARCH`, `TIMESERIES`, and `KV` statements are documented separately because some operations are commands rather than `SELECT` projections.

Examples of standalone multimodel routes:

```sql
document get app.documents key :document_id;
graph match app.graph using :pattern;
vector search app.product_embeddings using :query_vector limit 20;
search app.product_search for :search_text limit 25;
timeseries from app.metric_samples between :start_at and :end_at;
kv get app.session_cache key :session_key;
```

When those routes return a rowset, they may be consumed by `WITH`, a derived table, or another admitted query surface. When they perform a direct command, they remain outside `SELECT` and use their own operation family.

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
- Binding resolves catalog names, UUID references, parameter descriptors, result descriptors, security context, transaction context, and SBsql execution options.
- SBLR admission maps the bound request to an operation family and result shape.
- The engine rechecks authority before durable state changes or result delivery.
- Multimodel evidence sources are planner inputs only until the executor revalidates the final rowset.
- A query that combines relational and multimodel sources must bind every source to a compatible row descriptor before execution.
- A dedicated NoSQL command that does not return a rowset is not a `SELECT` source.

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
