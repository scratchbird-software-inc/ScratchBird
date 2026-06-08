# Window Clause And Window Functions

This page is part of the SBsql Language Reference Manual. It documents window specifications, named windows, partitioning, ordering, frame units, frame bounds, exclusions, ranking functions, offset functions, value functions, aggregate windows, descriptor behavior, optimizer constraints, and refusal cases.

Generation task: `syntax_reference_window`

Related pages: [SELECT Statement](select.md), [Projection](projection.md), [FROM And Table Expressions](from.md), [WHERE Clause](where.md), [GROUP BY And HAVING](group_by_and_having.md), [ORDER BY, LIMIT, OFFSET, And FETCH](order_by_limit_offset.md), [Operators](operators.md), [Type System Overview](../data_types/type_system_overview.md), and [Refusal Vectors](refusal_vectors.md).

## Purpose

Window functions compute values over a set of rows related to the current row while preserving one output row per input row. They are used for ranking, running totals, moving averages, offsets, percentiles, sessionization, gap analysis, and analytic reporting.

Example:

```sql
select customer_id,
       order_id,
       submitted_at,
       row_number() over (
         partition by customer_id
         order by submitted_at, order_id
       ) as customer_order_number
from app.orders;
```

The window function is descriptor-bound. Partition keys, order keys, frame bounds, null rules, collation, timezone behavior, aggregate state, and result descriptor are resolved before execution. SQL text does not own the analytic semantics after binding.

## Syntax

```ebnf
window_function_call ::=
    function_call OVER window_specification_or_name ;

window_specification_or_name ::=
      identifier
    | "(" window_specification ")" ;

window_clause ::=
    WINDOW window_definition ("," window_definition)* ;

window_definition ::=
    identifier AS "(" window_specification ")" ;
```

```ebnf
window_specification ::=
    base_window_name?
    partition_by_clause?
    window_order_by_clause?
    window_frame_clause? ;

base_window_name ::=
    identifier ;

partition_by_clause ::=
    PARTITION BY expression ("," expression)* ;

window_order_by_clause ::=
    ORDER BY sort_key ("," sort_key)* ;
```

```ebnf
window_frame_clause ::=
    frame_units frame_extent frame_exclusion? ;

frame_units ::=
      ROWS
    | RANGE
    | GROUPS ;

frame_extent ::=
      frame_start
    | BETWEEN frame_start AND frame_end ;

frame_start ::=
      UNBOUNDED PRECEDING
    | unsigned_integer_literal PRECEDING
    | CURRENT ROW
    | unsigned_integer_literal FOLLOWING ;

frame_end ::=
      CURRENT ROW
    | unsigned_integer_literal FOLLOWING
    | UNBOUNDED FOLLOWING
    | unsigned_integer_literal PRECEDING ;

frame_exclusion ::=
      EXCLUDE NO OTHERS
    | EXCLUDE CURRENT ROW
    | EXCLUDE GROUP
    | EXCLUDE TIES ;
```

SBsql is context sensitive. Window words are command words in window contexts and should not be treated as globally reserved identifiers elsewhere.

## Logical Position

Window functions are evaluated after `FROM`, `WHERE`, grouping, and `HAVING`, and before the final query `ORDER BY`, `LIMIT`, `OFFSET`, and `FETCH` result slicing.

This means:

- `WHERE` cannot directly reference a window-function result from the same query block;
- `HAVING` filters aggregate groups before window functions run;
- the final `ORDER BY` controls output order, not window frame order;
- window `ORDER BY` controls analytic order within each partition.

Use a derived table or CTE when a later clause must filter a window result:

```sql
with ranked_orders as (
  select customer_id,
         order_id,
         row_number() over (
           partition by customer_id
           order by submitted_at desc, order_id
         ) as rn
  from app.orders
)
select customer_id, order_id
from ranked_orders
where rn <= 3
order by customer_id, rn;
```

## Partitions

`PARTITION BY` divides the input rowset into independent analytic groups.

```sql
select customer_id,
       order_id,
       sum(total_amount) over (
         partition by customer_id
       ) as customer_total
from app.orders;
```

Partition expressions are descriptor-bound. Text partitions use collation; temporal partitions use precision and timezone policy; structured values require groupable descriptors.

If `PARTITION BY` is omitted, the entire visible input rowset is one partition.

## Window Ordering

Window `ORDER BY` defines the sequence of rows inside each partition.

```sql
select customer_id,
       order_id,
       submitted_at,
       sum(total_amount) over (
         partition by customer_id
         order by submitted_at, order_id
         rows between unbounded preceding and current row
       ) as running_total
from app.orders;
```

Ranking and offset functions that depend on order should use deterministic ordering. Add a stable tie-breaker when needed:

```sql
row_number() over (
  partition by customer_id
  order by submitted_at, order_id
)
```

Without an order key, row-numbering and offset results are not deterministic unless the function contract defines an order-independent result.

## Named Windows

The `WINDOW` clause defines reusable window specifications for the current query block:

```sql
select customer_id,
       order_id,
       row_number() over customer_order as rn,
       sum(total_amount) over customer_order as running_total
from app.orders
window customer_order as (
  partition by customer_id
  order by submitted_at, order_id
  rows between unbounded preceding and current row
);
```

Named windows are scope-local. They do not create catalog objects and cannot be referenced outside the query block that defines them.

A window specification may inherit a base window and refine it where the grammar and binder admit the refinement:

```sql
select order_id,
       sum(total_amount) over running_orders as running_total
from app.orders
window base_orders as (partition by customer_id order by submitted_at, order_id),
       running_orders as (base_orders rows between unbounded preceding and current row);
```

The binder must reject inheritance that creates ambiguous or conflicting partition, ordering, or frame definitions.

## Frame Units

The frame defines which rows inside the partition are visible to a frame-sensitive function for the current row.

| Unit | Meaning |
| --- | --- |
| `ROWS` | Counts physical rows relative to the current row after window ordering. |
| `RANGE` | Uses the value range around the current row's order key. Requires an order descriptor that supports range offsets. |
| `GROUPS` | Counts peer groups, where peers share the same window order key values. |

`ROWS` is the most explicit and portable frame unit. `RANGE` and `GROUPS` require additional descriptor support.

## Frame Bounds

Examples:

```sql
rows between unbounded preceding and current row
```

```sql
rows between 3 preceding and 3 following
```

```sql
range between interval '7' day preceding and current row
```

Frame bounds must be coherent. A frame start that logically follows its end is refused. Offset values must be non-negative and compatible with the frame unit and order descriptor.

If no explicit frame is supplied, the default frame is function-specific. Running aggregates usually use an order-sensitive frame when `ORDER BY` is present; whole-partition functions use the full partition. Scripts that require stable behavior should state the frame explicitly.

## Peer Groups And Exclusion

Rows with equal window order keys are peers. Peer groups affect `rank`, `dense_rank`, `percent_rank`, `cume_dist`, `RANGE`, `GROUPS`, and exclusion rules.

Frame exclusion controls whether certain rows are removed from the frame after bounds are computed:

| Exclusion | Meaning |
| --- | --- |
| `EXCLUDE NO OTHERS` | Keep all rows in the frame. |
| `EXCLUDE CURRENT ROW` | Remove the current row from the frame. |
| `EXCLUDE GROUP` | Remove the current row's peer group. |
| `EXCLUDE TIES` | Remove peer rows other than the current row. |

Unsupported exclusion forms must be refused before execution.

## Ranking Functions

| Function | Result |
| --- | --- |
| `row_number()` | Sequential number of the row inside the partition. |
| `rank()` | Rank with gaps for peer groups. |
| `dense_rank()` | Rank without gaps for peer groups. |
| `percent_rank()` | Relative rank from 0 to 1 where admitted. |
| `cume_dist()` | Cumulative distribution from greater than 0 to 1. |
| `ntile(n)` | Assigns rows into `n` buckets. |

Example:

```sql
select product_id,
       category_id,
       revenue,
       rank() over (
         partition by category_id
         order by revenue desc, product_id
       ) as revenue_rank
from app.product_revenue;
```

Ranking functions require an order descriptor for deterministic business meaning. Without explicit tie-breakers, peer ordering inside a rank may still be nondeterministic for functions that distinguish individual rows.

## Offset And Value Functions

| Function | Behavior |
| --- | --- |
| `lag(value [, offset [, default]])` | Reads a prior row in the window order. |
| `lead(value [, offset [, default]])` | Reads a following row in the window order. |
| `first_value(value)` | Reads the first value in the current frame. |
| `last_value(value)` | Reads the last value in the current frame. |
| `nth_value(value, n)` | Reads the nth value in the current frame. |

Example:

```sql
select account_id,
       event_at,
       balance,
       balance - lag(balance, 1, balance) over (
         partition by account_id
         order by event_at, event_id
       ) as balance_delta
from app.account_balance_event;
```

`last_value` is frame-sensitive. Use an explicit frame if the intended result is the last value in the whole partition:

```sql
last_value(balance) over (
  partition by account_id
  order by event_at, event_id
  rows between unbounded preceding and unbounded following
)
```

## Aggregate Window Functions

Aggregate functions can run as window functions:

```sql
select customer_id,
       order_id,
       total_amount,
       sum(total_amount) over (
         partition by customer_id
         order by submitted_at, order_id
         rows between unbounded preceding and current row
       ) as running_total,
       avg(total_amount) over (
         partition by customer_id
         rows between 2 preceding and current row
       ) as trailing_average
from app.orders;
```

Aggregate window state uses aggregate descriptors. Accumulator types may differ from the input and display descriptors. Memory limits, spill behavior, inverse transition support, and frame movement are operation-owned.

## Nulls, Collation, And Descriptors

Window behavior depends on descriptors:

| Area | Descriptor Rule |
| --- | --- |
| Partition equality | Uses descriptor equality, collation, canonicalization, and null grouping rules. |
| Window order | Uses descriptor ordering, collation, timezone, and null ordering rules. |
| Frame offsets | Require numeric, temporal, interval, or other descriptor support for the selected unit. |
| Aggregate state | Uses aggregate transition and result descriptors. |
| Value functions | Preserve or derive descriptors according to the function definition. |
| Protected values | Must not be exposed by ordering, diagnostics, or support output unless policy admits release. |

## Window Functions And GROUP BY

Window functions operate over the rowset produced after grouping when the query has aggregates:

```sql
select customer_id,
       sum(total_amount) as customer_total,
       rank() over (
         order by sum(total_amount) desc, customer_id
       ) as total_rank
from app.orders
group by customer_id;
```

The window sees one row per group. It does not see the pre-aggregate base rows unless the query block is structured to expose them.

## Optimizer And Execution

The optimizer may:

- reuse an input order that matches partition and order keys;
- sort by partition and order keys;
- use index order when descriptor and visibility semantics match;
- share compatible window partitions across multiple functions;
- spill window state when memory policy requires it;
- evaluate independent windows in separate physical phases.

The optimizer must not:

- change peer grouping by using the wrong collation or null ordering;
- evaluate volatile expressions in a way that changes observable behavior;
- expose rows hidden by RLS or masks;
- use index order without required final visibility and descriptor recheck;
- treat output `ORDER BY` as a substitute for window `ORDER BY`.

## Refusal And Diagnostics

| Condition | Result |
| --- | --- |
| Window function used where expressions are not admitted | Parse or bind diagnostic. |
| Unknown named window | Bind diagnostic. |
| Conflicting inherited window specification | Bind diagnostic. |
| Ranking or offset function lacks required ordering | Bind diagnostic or warning according to function contract. |
| Frame unit unsupported for order descriptor | `unsupported`. |
| Frame offset is negative or wrong type | Bind diagnostic. |
| Frame start follows frame end | Bind diagnostic. |
| Function cannot support requested frame or exclusion | `unsupported`. |
| Window state exceeds policy and cannot spill | `denied` or runtime diagnostic. |
| Protected value would be revealed in diagnostics | `denied` or redacted diagnostic. |

## Practical Examples

Top three orders per customer:

```sql
with ranked_orders as (
  select customer_id,
         order_id,
         total_amount,
         row_number() over (
           partition by customer_id
           order by total_amount desc, order_id
         ) as rn
  from app.orders
)
select customer_id, order_id, total_amount
from ranked_orders
where rn <= 3
order by customer_id, rn;
```

Running and trailing totals:

```sql
select account_id,
       event_at,
       amount,
       sum(amount) over (
         partition by account_id
         order by event_at, event_id
         rows between unbounded preceding and current row
       ) as running_amount,
       sum(amount) over (
         partition by account_id
         order by event_at, event_id
         rows between 6 preceding and current row
       ) as seven_event_amount
from app.account_event;
```

First and last values in a partition:

```sql
select account_id,
       event_id,
       first_value(event_at) over whole_account as first_event_at,
       last_value(event_at) over whole_account as last_event_at
from app.account_event
window whole_account as (
  partition by account_id
  order by event_at, event_id
  rows between unbounded preceding and unbounded following
);
```

## Verification Checklist

| Check | Required Outcome |
| --- | --- |
| Parse | Inline and named window specifications are recognized. |
| Bind | Partition, order, frame, function, and result descriptors resolve. |
| Determinism | Order-sensitive functions require or document deterministic ordering. |
| Frames | `ROWS`, `RANGE`, and `GROUPS` admit only compatible bounds and descriptors. |
| Peer groups | Rank and frame behavior uses the correct equality and collation rules. |
| Aggregates | Window aggregate state uses declared transition and result descriptors. |
| Security | RLS, masks, and protected values are enforced before analytic output. |
| Optimizer | Sort reuse, index order, and shared window phases preserve semantics. |
| Spill | Memory pressure uses admitted spill behavior or fails closed. |
| Proof | Full rebuild tests regenerate parser, SBLR, optimizer, executor, descriptor, and refusal evidence. |

## Related Surface Rows

| Surface | Kind | Family | Lowering | Result Shape |
| --- | --- | --- | --- | --- |
| `window_clause` | grammar production | query | yes | rowset descriptor |
| `window_definition` | grammar production | query | yes | window descriptor |
| `window_specification` | grammar production | query | yes | window descriptor |
| `partition_by_clause` | grammar production | query | yes | partition descriptor |
| `window_order_by_clause` | grammar production | query | yes | ordering descriptor |
| `window_frame_clause` | grammar production | query | yes | frame descriptor |
| `window_function_call` | expression | query | yes | scalar descriptor |
| `ranking_window_function` | function family | query | yes | numeric descriptor |
| `offset_window_function` | function family | query | yes | argument-derived descriptor |
| `aggregate_window_function` | function family | query | yes | aggregate result descriptor |
