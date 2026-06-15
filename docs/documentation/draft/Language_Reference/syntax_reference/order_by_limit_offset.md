# ORDER BY, LIMIT, OFFSET, And FETCH

This page is part of the SBsql Language Reference Manual. It explains the user-facing ordering and row-limiting contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `syntax_reference_order_limit`

Related pages: [SELECT Statement](select.md), [FROM And Table Expressions](from.md), [GROUP BY And HAVING](group_by_and_having.md), [WITH And Common Table Expressions](with.md), [Window Functions](window.md), [Indexes](index.md), [Operators And Precedence](operators.md), [Operator Type Result Matrix](operator_type_result_matrix.md), [Order By Clause EBNF](ebnf/order_by_clause.md), and [Limit Clause EBNF](ebnf/limit_clause.md).

## Purpose

`ORDER BY` defines the client-visible order of a rowset. Without `ORDER BY`, row order is not part of the query contract, even when the optimizer happens to use an index, a clustered access path, a temporary sort run, or a stable physical scan.

`LIMIT`, `OFFSET`, and `FETCH` restrict the number of rows delivered by a query. They do not change which rows satisfy `WHERE`, `GROUP BY`, `HAVING`, window, or authorization checks. They are applied after the rowset has been bound, filtered, grouped, projected where required by the query shape, ordered when an order is present, and admitted by SBLR.

Use an explicit deterministic `ORDER BY` whenever the row limit is semantically important. A limited query without a unique ordering is allowed, but repeated executions may return different peer rows when the plan, statistics, concurrent data, or visible snapshot changes.

```sql
select order_id,
       customer_id,
       submitted_at,
       order_total
from app.orders
where order_state = 'open'
order by submitted_at desc, order_id
limit 50 offset 100;
```

## Syntax

The compact generated grammar exposes these productions:

```ebnf
order_by_clause ::= "ORDER" "BY" ordering_list ;
limit_clause    ::= "LIMIT" expression ("OFFSET" expression)? ;
```

The full SBsql clause shape is:

```ebnf
order_by_clause ::=
    "ORDER" "BY" sort_spec ("," sort_spec)* ;

sort_spec ::=
    sort_expression sort_direction? null_ordering? ;

sort_expression ::=
      expression
    | result_column_name
    | result_column_ordinal ;

sort_direction ::=
      "ASC"
    | "DESC" ;

null_ordering ::=
      "NULLS" "FIRST"
    | "NULLS" "LAST" ;

limit_clause ::=
      "LIMIT" limit_count ("OFFSET" offset_count)?
    | fetch_clause ;

fetch_clause ::=
    "FETCH" fetch_quantifier? limit_count "ROW" row_plural? "ONLY" ;

fetch_quantifier ::=
      "FIRST"
    | "NEXT" ;

row_plural ::=
    "S" ;

limit_count ::=
    expression ;

offset_count ::=
    expression ;
```

`FETCH FIRST n ROWS ONLY` and `FETCH NEXT n ROW ONLY` bind to the same bounded row-limit route. `FETCH ... WITH TIES` and percent-based fetch are not part of the public SBsql contract described here; unsupported fetch variants must fail closed with a diagnostic message vector.

## Clause Position

In a simple `SELECT`, ordering and row limiting appear after filtering, grouping, `HAVING`, and window declarations:

```ebnf
select_statement ::=
    "SELECT" set_quantifier? projection_list
    from_clause?
    where_clause?
    group_by_clause?
    having_clause?
    window_clause?
    order_by_clause?
    limit_clause? ;
```

For a `WITH` query, the final `ORDER BY`, `LIMIT`, `OFFSET`, or `FETCH` belongs to the query expression that follows the CTE declarations unless it appears inside a nested subquery.

```sql
with recent_orders as (
  select order_id, customer_id, submitted_at
  from app.orders
  where submitted_at >= :start_at
)
select order_id, customer_id
from recent_orders
order by submitted_at desc, order_id
fetch first 25 rows only;
```

## ORDER BY Binding

Each sort item is bound independently. The binder resolves the sort expression against the query result descriptor and visible query scope.

| Sort item | Binding rule |
| --- | --- |
| Output alias | Resolves to the matching projected column when the alias is unique in the result descriptor. |
| Output column name | Resolves to the projected column or visible source column according to the query scope. Ambiguity is refused. |
| Ordinal | Resolves to a one-based projected column position. Out-of-range ordinals are refused. |
| Expression | Binds as an ordinary expression and must have a sortable descriptor. |

Examples:

```sql
select customer_id,
       sum(order_total) as lifetime_value
from app.orders
group by customer_id
order by lifetime_value desc, customer_id;
```

```sql
select order_id, submitted_at
from app.orders
order by 2 desc, 1;
```

Ordinal ordering is useful for short ad hoc queries, but named ordering is clearer in long-lived scripts. If a result column is added or moved, ordinal meaning changes.

## Sort Direction

`ASC` orders from the lowest value to the highest value. `DESC` orders from the highest value to the lowest value. When direction is omitted, SBsql uses ascending order.

```sql
select order_id, submitted_at
from app.orders
order by submitted_at desc, order_id asc;
```

Multi-column ordering is lexicographic. SBsql compares the first sort key; if two rows are peers for that key, it compares the second key; it continues through the remaining keys. Rows that compare equal for every sort key are peers.

For stable pagination, include a final key that is unique within the visible result set.

```sql
select order_id, submitted_at
from app.orders
where order_state = 'open'
order by submitted_at desc, order_id
limit 50;
```

In that example, `submitted_at` provides the business order and `order_id` breaks ties.

## Null Ordering

`NULLS FIRST` places `NULL` values before non-null values for that sort item. `NULLS LAST` places `NULL` values after non-null values.

```sql
select customer_id, last_contact_at
from app.customers
order by last_contact_at nulls last, customer_id;
```

When explicit null ordering is omitted, the effective behavior is the descriptor default for the bound type and collation. Scripts that need portable, repeatable ordering should spell `NULLS FIRST` or `NULLS LAST`.

`NULL` comparison for sorting is not the same as predicate comparison. In predicates, `NULL` participates in three-valued logic. In ordering, `NULL` placement is a sorting policy.

## Collation, Charset, And Type Rules

Sorting uses the bound descriptor for each sort expression.

| Value family | Ordering rule |
| --- | --- |
| Integer and exact numeric values | Numeric order after descriptor-compatible coercion. |
| Floating values | Numeric order under the descriptor's floating-value policy. Non-finite values use the descriptor policy for that type. |
| Text values | Bound charset and collation decide comparison, case behavior, accent behavior, and normalization behavior. |
| Binary values | Byte-order comparison unless a richer descriptor-specific operation is declared. |
| UUID values | UUID descriptor order, not raw text spelling. Time-ordered UUID descriptors may expose a different key order from ordinary UUID text formatting. |
| Temporal values | Temporal instant or local-field ordering according to the bound temporal descriptor. |
| Boolean values | Descriptor-defined boolean order. |
| Document, graph, vector, protected, and structured values | Not directly sortable unless the descriptor exposes a sortable projection or operation. |

Use explicit scalar expressions when sorting structured values.

```sql
select document_id,
       json_value(body, '$.priority') as priority_text
from app.inbox_documents
where json_exists(body, '$.priority')
order by cast(priority_text as int32), document_id;
```

The parser does not choose text, numeric, temporal, or binary behavior from the spelling of a value. The binder chooses behavior from descriptors and declared coercion rules.

## LIMIT

`LIMIT` returns at most the requested number of rows.

```sql
select order_id, submitted_at
from app.orders
order by submitted_at desc, order_id
limit 10;
```

The limit expression must bind to a non-negative integral value. A value of zero returns an empty rowset. Negative, fractional, nonnumeric, overflowed, or otherwise non-integral limits are refused.

`LIMIT` is evaluated after the query route has produced its visible, authorized rowset. The optimizer may use the bound limit to choose a top-N plan or ordered index route, but that optimization does not change row visibility or authorization.

## OFFSET

`OFFSET` skips the requested number of rows after ordering.

```sql
select order_id, submitted_at
from app.orders
order by submitted_at desc, order_id
limit 25 offset 50;
```

The offset expression must bind to a non-negative integral value. A value of zero skips no rows. If the offset is greater than or equal to the row count, the query returns an empty rowset.

Offset pagination is simple, but it can become expensive for large offsets because skipped rows may still need to be found, ordered, authorized, and counted. For large result sets, prefer keyset pagination where possible.

```sql
select order_id, submitted_at
from app.orders
where (submitted_at, order_id) < (:last_submitted_at, :last_order_id)
order by submitted_at desc, order_id desc
limit 25;
```

The keyset predicate must use the same logical ordering keys as the `ORDER BY` clause.

## FETCH

`FETCH` is the standard row-limiting spelling for cases where a script wants a bounded number of rows without using `LIMIT`.

```sql
select order_id, submitted_at
from app.orders
order by submitted_at desc, order_id
fetch first 10 rows only;
```

`FIRST` and `NEXT` are equivalent in SBsql row-limiting clauses:

```sql
select order_id
from app.orders
fetch next 5 row only;
```

`ROW` and `ROWS` are accepted for readability. The count expression follows the same binding and refusal rules as `LIMIT`.

## Determinism And Peer Rows

A row limit is deterministic only when the final ordering is deterministic for the visible snapshot.

```sql
select customer_id, order_id, submitted_at
from app.orders
order by submitted_at desc
limit 5;
```

This query is deterministic only if `submitted_at` is unique for every visible row. If two or more rows share the same timestamp, any of those peer rows may appear at the boundary.

Add a tie breaker:

```sql
select customer_id, order_id, submitted_at
from app.orders
order by submitted_at desc, order_id
limit 5;
```

The same rule applies to search rank, vector distance, graph traversal depth, grouped aggregates, and computed expressions. If peers can exist, add a deterministic final key.

## ORDER BY With DISTINCT And GROUP BY

When `DISTINCT` is used, `ORDER BY` applies to the distinct result rows.

```sql
select distinct customer_id
from app.orders
order by customer_id;
```

When `GROUP BY` is used, `ORDER BY` applies to grouped rows and may refer to grouping expressions or aggregate outputs that are visible in the result descriptor.

```sql
select customer_id,
       count(*) as order_count,
       max(submitted_at) as last_order_at
from app.orders
group by customer_id
having count(*) > 0
order by last_order_at desc, customer_id
limit 100;
```

An `ORDER BY` expression that cannot be bound to the grouped row descriptor is refused.

## ORDER BY With Window Functions

Window functions can have their own `ORDER BY` inside `OVER (...)`. That order controls the window frame or function evaluation for each partition. The final query `ORDER BY` controls the delivered row order.

```sql
select customer_id,
       order_id,
       row_number() over (
         partition by customer_id
         order by submitted_at desc, order_id
       ) as customer_order_rank
from app.orders
order by customer_id, customer_order_rank;
```

The window order and the final result order may be the same, but they are separate contracts.

## ORDER BY In Subqueries And CTEs

An `ORDER BY` inside a derived table, CTE, or subquery is meaningful only when the nested query's contract admits ordered delivery to its consumer, usually because the nested query also has `LIMIT`, `OFFSET`, `FETCH`, a cursor-producing route, or another ordered rowset contract.

```sql
with latest_orders as (
  select order_id, customer_id, submitted_at
  from app.orders
  order by submitted_at desc, order_id
  limit 100
)
select customer_id, count(*) as recent_order_count
from latest_orders
group by customer_id
order by recent_order_count desc, customer_id;
```

Do not rely on an inner `ORDER BY` to control the final order of an outer query. The outer query needs its own `ORDER BY`.

## ORDER BY In DML Sources

`INSERT ... SELECT`, `MERGE`, and other row-consuming statements can consume a query that has `ORDER BY`, `LIMIT`, `OFFSET`, or `FETCH` when that source query is admitted by the binder.

```sql
insert into app.recent_order_queue(order_id, submitted_at)
select order_id, submitted_at
from app.orders
where order_state = 'open'
order by submitted_at desc, order_id
limit 100;
```

Ordering a source query does not give the statement transaction finality. The target mutation remains governed by MGA, constraints, triggers, authorization, and the target statement route.

## Multimodel Ordering

Rowset-producing document, graph, vector, search, time-series, and key-value projections can be ordered when the projected sort expressions bind to sortable descriptors.

```sql
select product_id,
       l2_distance(embedding, vector(:query_embedding)) as distance
from app.product_embeddings
where vector_dims(embedding) = vector_dims(vector(:query_embedding))
order by distance, product_id
limit 20;
```

```sql
select document_id,
       json_value(body, '$.customer.id') as customer_id
from app.order_documents
where json_exists(body, '$.customer.id')
order by customer_id, document_id
limit 50;
```

Search scores, vector distances, graph traversal depth, time buckets, and document paths are evidence or projected values until bound to result descriptors. Final row delivery still requires descriptor compatibility, visibility checks, and authorization.

## Optimizer And Execution

The optimizer may satisfy `ORDER BY`, `LIMIT`, `OFFSET`, or `FETCH` through several admitted strategies:

| Strategy | When it may be used | Contract |
| --- | --- | --- |
| Ordered index access | A usable index matches the order keys and visibility checks can be preserved. | Index order is candidate evidence; the executor still rechecks row visibility and predicates. |
| Top-N execution | A limit is present and a bounded sort can produce the first N rows. | The bound is part of the plan and must be enforced. |
| Temporary sort | The rowset must be sorted after scan, join, grouping, or projection. | Sort runs must preserve descriptors, row identity, cancellation, and spill policy. |
| Streaming order | The producer already emits rows in an admitted order. | The route must prove that its output order matches the requested sort contract. |
| Final slice | `LIMIT` or `OFFSET` is applied after order and visibility. | The slice must not be applied before authorization or predicate truth is established. |

An index can avoid a separate sort, but it does not become row authority. The executor must still verify MGA visibility, predicate truth, descriptor compatibility, and security before returning each row.

## Error And Refusal Cases

SBsql must fail closed for invalid ordering and row limiting. Common refusal cases include:

| Case | Expected behavior |
| --- | --- |
| Unknown sort column or alias | Refuse with a name-resolution diagnostic. |
| Ambiguous sort name | Refuse with an ambiguity diagnostic. |
| Out-of-range ordinal | Refuse with a binding diagnostic. |
| Unsuitable sort descriptor | Refuse with a descriptor or operation diagnostic. |
| Negative limit or offset | Refuse with a value-domain diagnostic. |
| Fractional or nonnumeric limit or offset | Refuse with a type diagnostic. |
| Overflowed limit or offset | Refuse with a value-domain diagnostic. |
| Unsupported fetch variant | Refuse with an unsupported-feature diagnostic. |
| Hidden or unauthorized sort expression | Refuse with a security diagnostic. |
| Planner cannot preserve required order | Use a safe fallback sort or refuse if policy disallows it. |

Diagnostics are returned as message vectors. A parser-side acceptance is not execution authority.

## Practical Patterns

Top rows:

```sql
select order_id, order_total
from app.orders
order by order_total desc, order_id
limit 10;
```

Paged result:

```sql
select order_id, submitted_at
from app.orders
order by submitted_at desc, order_id
limit :page_size offset :page_start;
```

Keyset pagination:

```sql
select order_id, submitted_at
from app.orders
where (submitted_at, order_id) < (:last_submitted_at, :last_order_id)
order by submitted_at desc, order_id desc
limit :page_size;
```

Bounded grouped report:

```sql
select customer_id,
       sum(order_total) as total_value
from app.orders
group by customer_id
order by total_value desc, customer_id
fetch first 100 rows only;
```

Stable export chunk:

```sql
select order_id, customer_id, submitted_at, order_total
from app.orders
where order_id > :last_seen_order_id
order by order_id
limit :chunk_size;
```

## Proof Expectations

Public proof for this surface should include:

- parser acceptance for contextual `ORDER`, `LIMIT`, `OFFSET`, and `FETCH` tokens;
- exact lowering to the admitted query rowset route;
- SBLR envelope verification for ordered select and fetch-limited select;
- row-result proof that descending order, limit, and offset produce the expected row count and row order;
- binary round-trip proof for the route payload;
- refusal proof for unresolved names, invalid descriptors, invalid limits, unsupported fetch variants, and authorization failures;
- optimizer proof that index order, top-N, and temporary sort remain evidence and never bypass MGA visibility or predicate recheck.

The visible public route evidence includes ordered `SELECT ... ORDER BY id DESC LIMIT 2 OFFSET 1` and `SELECT ... FETCH FIRST 2 ROWS ONLY` fixtures. Those fixtures prove the bounded route, contextual keyword recognition, SBLR admission, server dispatch, and MGA row-result behavior for the public surface.
