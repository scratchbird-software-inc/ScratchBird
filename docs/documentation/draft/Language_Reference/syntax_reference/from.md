# FROM And Table Expressions

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `syntax_reference_from`

Related pages: [SELECT Statement](select.md), [WITH And Common Table Expressions](with.md), [WHERE Clause](where.md), [GROUP BY And HAVING](group_by_and_having.md), [Views](view.md), [Functions](function.md), [COPY Streaming Import And Export](copy.md), [Schema Tree And Name Resolution](schema_tree_and_name_resolution.md), and [Document, Graph, Vector, And Multimodel Types](../data_types/document_graph_vector_and_multimodel_types.md).

## Purpose

`FROM` declares the row sources for a query. A row source can be a base table, view, common table expression, derived table, table function, value constructor, bridge relation, catalog projection, or descriptor-bound multimodel projection. Each source must bind to a row descriptor before the query can execute.

The `FROM` clause is not storage authority. Names are resolver input. The binder resolves sources to catalog UUIDs, descriptors, parameters, row aliases, column aliases, security context, and transaction context. The optimizer may choose access paths and join order, but the executor still rechecks visibility, authorization, descriptor compatibility, and predicates before returning rows.

## Syntax

```ebnf
from_clause ::=
    FROM table_expression ;

table_expression ::=
      table_reference ("," table_reference)*
    | joined_table ;

table_reference ::=
      relation_reference
    | derived_table
    | table_function_reference
    | values_table
    | multimodel_table_reference
    | bridge_table_reference ;

relation_reference ::=
    qualified_name table_alias? ;

derived_table ::=
    "(" query_dml_stmt ")" table_alias column_alias_list? ;

table_function_reference ::=
    TABLE "(" function_call ")" table_alias? column_alias_list? ;

values_table ::=
    VALUES row_constructor ("," row_constructor)* table_alias column_alias_list? ;

joined_table ::=
    table_reference join_clause+ ;

join_clause ::=
      join_type? JOIN table_reference join_condition
    | CROSS JOIN table_reference
    | LATERAL JOIN table_reference join_condition? ;

join_type ::=
      INNER
    | LEFT OUTER?
    | RIGHT OUTER?
    | FULL OUTER? ;

join_condition ::=
      ON predicate
    | USING "(" identifier ("," identifier)* ")" ;

table_alias ::=
    AS? identifier ;

column_alias_list ::=
    "(" identifier ("," identifier)* ")" ;
```

SBsql is context sensitive. `FROM`, `JOIN`, `ON`, `USING`, `TABLE`, and `LATERAL` are command words inside table expressions and should not be treated as globally reserved identifiers outside this context.

## Source Families

| Source family | Example shape | Binding rule |
| --- | --- | --- |
| Base table | `app.orders o` | Resolves to a table UUID and row descriptor. |
| View | `app.open_orders o` | Resolves to the view descriptor and admitted expansion/execution route. |
| CTE | `recent_orders r` | Resolves to a `WITH` query descriptor in the statement scope. |
| Derived table | `(select ... ) x` | Requires an alias and a known result descriptor. |
| Table function | `table(app.split_text(:text)) s` | Function must return a rowset descriptor. |
| Values table | `values (1, 'a'), (2, 'b') v(id, label)` | Row constructors define an inline rowset descriptor. |
| Catalog projection | `sys.catalog_type_descriptor t` | Reads authorized catalog projection rows. |
| Bridge relation | `app.remote_orders r` | Reads through an admitted bridge relation descriptor. |
| Document projection | `app.document_rows d` | Exposes descriptor-bound document rows or fields. |
| Key-value projection | `app.session_cache c` | Exposes key, value, version, TTL, or metadata descriptors. |
| Graph projection | `app.graph_edge e` | Exposes node, edge, path, or property row descriptors. |
| Vector projection | `app.product_embedding e` | Exposes vector-bearing rows and metric-compatible descriptors. |
| Search projection | `app.product_search_hits h` | Exposes ranked search-hit descriptors. |
| Time-series projection | `app.metric_sample m` | Exposes series, timestamp, bucket, and value descriptors. |

Every source is a rowset from the query binder's point of view. A source may be backed by ordinary storage, a virtual projection, a function, a stream, or a bridge, but execution still requires a row descriptor and an admitted SBLR route.

## Base Tables And Views

```sql
select o.order_id,
       o.customer_id,
       o.total_amount
from app.orders o
where o.order_state = 'open';
```

The table name `app.orders` resolves through the active schema rules. Alias `o` becomes the preferred qualifier for columns from that source. If an alias is supplied, unqualified uses of the original table name in the same query block are not a separate authority path.

Views are read through their descriptor and admitted expansion route.

```sql
select o.order_id, o.customer_name
from app.open_order_view o
where o.customer_id = :customer_id;
```

The caller needs privilege on the visible view surface and any required base-object privileges according to the view's security mode and policy.

## Joins

Joins combine row sources. Join predicates are descriptor-bound expressions and cannot bypass row visibility or authorization.

```sql
select c.customer_name,
       o.order_id,
       o.total_amount
from app.customer c
join app.orders o on o.customer_id = c.customer_id
where o.order_state = 'open'
order by c.customer_name, o.order_id;
```

Outer joins preserve unmatched rows from one or both sides and fill missing columns with SQL `NULL` according to the joined row descriptor.

```sql
select c.customer_id,
       c.customer_name,
       o.order_id
from app.customer c
left join app.orders o on o.customer_id = c.customer_id;
```

`USING` joins require same-named columns that can bind to compatible descriptors.

```sql
select customer_id, order_id, total_amount
from app.customer
join app.orders using (customer_id);
```

`CROSS JOIN` forms a product of both sources.

```sql
select r.region_code, p.product_id
from app.region r
cross join app.product p;
```

## Derived Tables And CTEs

Derived tables are nested queries in `FROM`.

```sql
select x.customer_id, x.order_total
from (
  select customer_id, sum(total_amount) as order_total
  from app.orders
  group by customer_id
) x
where x.order_total > 1000;
```

A derived table must have an alias. Column aliases can override the derived result names.

```sql
select s.id, s.label
from (
  values (1, 'open'), (2, 'closed')
) s(id, label);
```

CTEs are named query expressions introduced by `WITH`.

```sql
with recent_orders as (
  select order_id, customer_id, total_amount
  from app.orders
  where submitted_at >= :start_at
)
select r.order_id, c.customer_name
from recent_orders r
join app.customer c on c.customer_id = r.customer_id;
```

Recursive CTE rules are documented in [WITH And Common Table Expressions](with.md).

## Table Functions

A table function is a function call that returns a rowset descriptor.

```sql
select token_value, token_ordinal
from table(app.tokenize(:input_text)) t(token_value, token_ordinal);
```

Table functions must declare their result descriptor. If a function returns a scalar value, it is an expression and not a table source.

`LATERAL` lets a source refer to columns from a prior source in the same `FROM` clause when the function or derived table admits correlation.

```sql
select d.document_id, p.path_name, p.path_value
from app.document_store d
lateral join table(app.document_paths(d.document_body)) p(path_name, path_value) on true;
```

The correlated source is evaluated according to the optimizer's admitted plan, but descriptor binding and authorization still happen before execution.

## Multimodel Row Sources

Descriptor-bound multimodel projections can participate in `FROM` when they expose rows.

Document projection:

```sql
select d.document_key,
       json_value(d.document_body, '$.status') as status
from app.document_store d
where json_exists(d.document_body, '$.status');
```

Vector projection:

```sql
select p.product_id,
       l2_distance(e.embedding, vector(:query_embedding)) as distance
from app.product p
join app.product_embedding e on e.product_id = p.product_id
order by distance, p.product_id
limit 20;
```

Search-hit projection:

```sql
select p.product_id, h.score
from app.product_search_hits h
join app.product p on p.object_uuid = h.object_uuid
where h.query_text = :query_text
order by h.score desc, p.product_id;
```

A search, vector, graph, or document index can produce candidates. It cannot become final row authority. The executor must recheck visibility, descriptor compatibility, predicates, and authorization.

## Bridge Row Sources

A bridge relation can appear in `FROM` when an authorized bridge connection exposes a rowset descriptor.

```sql
select local_o.order_id,
       remote_o.external_status
from app.orders local_o
join app.remote_order_status remote_o on remote_o.order_id = local_o.order_id;
```

A bridge row source is a connection boundary. It does not move transaction finality out of the participating databases. Local transaction visibility remains local MGA authority; remote visibility is governed by the remote endpoint and bridge policy.

## Mixed Relational And Multimodel Queries

SBsql treats relational tables, document projections, key-value projections, graph projections, search hits, time-series samples, vector rows, table functions, and bridge relations as row sources only after they expose row descriptors. This lets one query combine different storage models without letting any one model bypass the ordinary query contract.

```sql
select c.customer_id,
       c.customer_name,
       json_value(p.profile_document, '$.tier') as tier_name,
       s.score as search_score
from app.customer c
join app.customer_profile p on p.customer_id = c.customer_id
join app.customer_search_hits s on s.object_uuid = c.object_uuid
where s.query_text = :query_text
  and json_exists(p.profile_document, '$.tier')
order by s.score desc, c.customer_id;
```

In this example:

- `app.customer` is an ordinary row source.
- `app.customer_profile` exposes document-bearing rows.
- `app.customer_search_hits` exposes ranked candidate rows.
- `json_value` and `json_exists` are descriptor-bound expressions over the document value.
- `ORDER BY` is still required because `FROM` and search ranking alone do not define final result order.

Candidate row sources can narrow the work the executor performs. They cannot replace final descriptor, visibility, authorization, and predicate checks.

## Column Shape And Alias Rules

Each table expression contributes a row descriptor to the query block. The output shape of the `FROM` clause is the combined descriptor after join and alias processing.

| Rule | Contract |
| --- | --- |
| Source alias | If an alias is supplied, it becomes the visible qualifier for that source in the query block. |
| Column alias list | Overrides the visible column names of a derived table, values table, or table function result. |
| Duplicate names | Must be qualified or renamed before unqualified references can bind safely. |
| Hidden columns | Remain hidden unless the source explicitly exposes them through an authorized projection. |
| Generated columns | Bind through their declared descriptors and dependencies. |
| Protected columns | Require policy admission before they can appear in expressions or result descriptors. |
| Structured columns | Expose fields only through admitted descriptor operations. |

Example with explicit column aliases:

```sql
select src.order_id, src.total_amount
from (
  values (:order_id, :amount)
) src(order_id, total_amount);
```

## Correlation And Lateral Evaluation

Correlation lets a nested row source refer to columns from an outer query block or from a prior source in the same `FROM` clause. SBsql requires correlation to be explicit where the table expression would otherwise be ambiguous.

```sql
select o.order_id,
       item_rows.item_count
from app.orders o
lateral join table(app.order_item_summary(o.order_id)) item_rows(item_count) on true;
```

The correlated function receives `o.order_id` as a typed value. It does not receive unbound text or direct catalog authority. Correlated execution must still obey cancellation, resource, sandbox, and transaction policy.

## Comma Sources And Join Predicates

A comma-separated source list is a table expression list. It should be used only when the intended product is clear or when predicates are applied in `WHERE`.

```sql
select c.customer_name, o.order_id
from app.customer c, app.orders o
where o.customer_id = c.customer_id;
```

The explicit `JOIN ... ON` form is preferred for new documentation and examples because it keeps join predicates attached to the join they describe.

## Name Resolution And Scope

Resolution order is scoped to the query block:

1. local table aliases;
2. CTE names in the current `WITH` scope;
3. visible schema objects through schema resolution rules;
4. authorized virtual projections and bridge descriptors;
5. function/table-function names where the grammar expects a function call.

Column references should be qualified when more than one source exposes the same column name. Ambiguous unqualified names must be refused.

## Optimizer And Execution

The optimizer may reorder joins, push predicates, use indexes, materialize derived tables, stream table functions, or choose hash/sort/merge join strategies where admitted. These choices do not change the language result.

| Concern | Rule |
| --- | --- |
| Visibility | Every row read must pass MGA snapshot visibility. |
| Authorization | Every source and column must be authorized. |
| Predicate truth | `ON` and `WHERE` predicates are descriptor-bound boolean expressions. |
| Nulls | Outer joins produce `NULL` for missing side columns. |
| Order | `FROM` does not define output order; use `ORDER BY`. |
| Candidate evidence | Indexes, search hits, vector candidates, and graph traversal evidence require final row recheck. |
| Remote input | Bridge results are input rows, not local storage or transaction authority. |

## Proof Expectations

The `FROM` proof suite should include:

- table, view, CTE, derived-table, values-table, table-function, multimodel, and bridge row sources;
- inner, outer, cross, `USING`, and correlated lateral joins;
- alias replacement, duplicate column names, hidden column refusal, and column-alias lists;
- mixed relational and multimodel queries that prove candidate evidence is rechecked;
- sandboxed schema roots that hide sources outside the effective root;
- bridge row sources that preserve local and remote transaction boundaries;
- recovery-required and policy-denied refusals before row execution.

## Diagnostics And Refusals

| Condition | Expected diagnostic class |
| --- | --- |
| Source not found or hidden by sandbox | Object resolution or sandbox denied. |
| Ambiguous source or column name | Name resolution failure. |
| Missing read privilege | Authorization denied. |
| Derived table missing alias | Syntax or binding failure. |
| Table function returns scalar | Descriptor mismatch. |
| Join columns in `USING` are incompatible | Descriptor mismatch. |
| Bridge relation unavailable | Bridge unavailable or policy denied. |
| Cluster-classified distributed query requested without admission | Cluster-gated refusal. |
| Recovery-required state | Operation fenced until recovery action completes. |

## Verification Checklist

| Check | Required outcome |
| --- | --- |
| Parse | `FROM` and table-expression shape is recognized by SBsql. |
| Bind | Sources, aliases, columns, descriptors, parameters, and correlation scopes resolve. |
| Authorize | Effective user or agent UUID may read every source and visible column. |
| Admit | SBLR query route and result shape are accepted by the engine verifier. |
| Optimize | Access paths and join order preserve descriptor and predicate semantics. |
| Execute | Rows are rechecked for visibility, authorization, and predicate truth. |
| Render | Result descriptors expose only authorized columns and values. |
