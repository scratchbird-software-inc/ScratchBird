# GROUP BY And HAVING

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `syntax_reference_group_by_having`

Related pages: [SELECT Statement](select.md), [FROM And Table Expressions](from.md), [WHERE Clause](where.md), [Operators](operators.md), [Operator Type Result Matrix](operator_type_result_matrix.md), [Type System Overview](../data_types/type_system_overview.md), and [Core Functions](../functional_reference/sb_core.md).

## Purpose

`GROUP BY` partitions visible input rows into aggregate groups. `HAVING` filters those groups after aggregate state is available. The grouping expressions, aggregate arguments, accumulator descriptors, collation rules, null rules, and result descriptors must bind before execution.

`WHERE` filters rows before grouping. `HAVING` filters groups after grouping. Aggregate functions can appear in `HAVING` and the select list; ordinary non-aggregate expressions in an aggregate query must be functionally dependent on the grouping keys or appear in the grouping key list.

## Syntax

```ebnf
group_by_clause ::=
    GROUP BY group_by_item ("," group_by_item)* group_by_modifier? ;

group_by_item ::=
      expression
    | column_ordinal
    | grouping_set ;

group_by_modifier ::=
      WITH ROLLUP
    | WITH CUBE ;

grouping_set ::=
      GROUPING SETS "(" grouping_set_list ")"
    | ROLLUP "(" expression_list ")"
    | CUBE "(" expression_list ")" ;

having_clause ::=
    HAVING predicate ;
```

Advanced grouping forms are admitted only when the active SBsql profile and engine route define the required result descriptors and grouping-id behavior.

## Basic Grouping

```sql
select customer_id,
       count(*) as order_count,
       sum(total_amount) as order_total
from app.orders
where order_state = 'closed'
group by customer_id
having sum(total_amount) > 1000
order by customer_id;
```

Execution order is conceptually:

1. bind `FROM` sources;
2. apply `WHERE` to visible rows;
3. compute grouping keys;
4. build aggregate state for each group;
5. apply `HAVING`;
6. project result expressions;
7. apply `ORDER BY`, `LIMIT`, and `OFFSET`.

The optimizer may choose a different physical plan when it preserves those semantics.

## Grouping Keys

Grouping keys are descriptor-bound expressions.

```sql
select date_trunc('day', submitted_at) as submitted_day,
       count(*) as order_count
from app.orders
group by date_trunc('day', submitted_at)
order by submitted_day;
```

| Key type | Rule |
| --- | --- |
| Column reference | Binds to a source column descriptor. |
| Expression | Binds to an expression descriptor and dependency graph. |
| Text value | Uses the bound charset and collation descriptor. |
| Numeric value | Uses numeric equality and canonicalization rules for the descriptor. |
| Temporal value | Uses timestamp/date/time descriptor precision and time-zone rules. |
| Structured value | Requires an admitted equality/grouping descriptor. |
| `NULL` | All SQL `NULL` grouping-key values for the same key position belong to one null group. |

Grouping by output alias is allowed only where the binder can prove the alias maps unambiguously to the same expression in the current query block.

## Aggregate Functions

Aggregate functions operate on groups and may use accumulator descriptors that differ from the final display descriptor.

```sql
select customer_id,
       count(*) as row_count,
       count(discount_code) as discounted_count,
       sum(total_amount) as total_amount,
       avg(total_amount) as average_amount,
       min(submitted_at) as first_order_at,
       max(submitted_at) as last_order_at
from app.orders
group by customer_id;
```

| Aggregate | Null behavior |
| --- | --- |
| `count(*)` | Counts input rows. |
| `count(expression)` | Counts non-null expression values. |
| `sum(expression)` | Aggregates non-null values and returns null for an empty non-null set unless a function-specific rule states otherwise. |
| `avg(expression)` | Uses sum/count accumulator rules and returns null for an empty non-null set. |
| `min(expression)` / `max(expression)` | Ignores null values and uses descriptor ordering rules. |
| Collection aggregates | Require an admitted result descriptor, memory policy, ordering policy, and size limits. |

`DISTINCT` aggregate arguments are admitted where the aggregate function defines a distinct accumulator.

```sql
select customer_id,
       count(distinct product_id) as product_count
from app.order_item
group by customer_id;
```

## Aggregate Return Descriptors

Aggregate result types are descriptor-defined. The following table describes the ordinary SBsql contract; exact precision, scale, collation, and nullability are governed by the input descriptors and aggregate definition.

| Aggregate form | Input descriptor | Result descriptor contract |
| --- | --- | --- |
| `count(*)` | Row presence | Exact integer count, not nullable for an executed group. |
| `count(value)` | Any descriptor | Exact integer count of non-null values. |
| `sum(integer)` | Integer family | Widened exact numeric accumulator where admitted. |
| `sum(decimal)` | Decimal family | Decimal result with declared precision/scale growth rules. |
| `sum(real)` | Approximate numeric | Approximate numeric result using the descriptor's floating rules. |
| `avg(integer)` | Integer family | Decimal or exact numeric average descriptor where admitted. |
| `avg(decimal)` | Decimal family | Decimal average descriptor with defined precision/scale. |
| `avg(real)` | Approximate numeric | Approximate numeric average descriptor. |
| `min` / `max` | Ordered descriptor | Same descriptor family as input, using descriptor ordering rules. |
| Collection aggregate | Structured or scalar | Declared collection descriptor with memory and size policy. |

Queries that need a specific display descriptor should cast explicitly.

```sql
select customer_id,
       cast(avg(total_amount) as decimal(18,2)) as average_amount
from app.orders
group by customer_id;
```

## HAVING

`HAVING` filters groups after aggregate state has been computed.

```sql
select customer_id,
       sum(total_amount) as total_amount
from app.orders
group by customer_id
having count(*) >= 5
   and sum(total_amount) > 1000;
```

`HAVING` predicates may reference grouping keys, aggregate functions, and deterministic expressions over those values. A `HAVING` predicate cannot refer to an arbitrary non-grouped source column unless the binder proves it is functionally dependent on the group.

Use `WHERE` when filtering individual rows before grouping:

```sql
select customer_id, sum(total_amount)
from app.orders
where order_state = 'closed'
group by customer_id;
```

Use `HAVING` when filtering aggregate groups:

```sql
select customer_id, sum(total_amount)
from app.orders
group by customer_id
having sum(total_amount) > 1000;
```

## Grouping Without GROUP BY

An aggregate query without `GROUP BY` has one implicit group over the visible input rows.

```sql
select count(*) as order_count,
       sum(total_amount) as total_amount
from app.orders
where order_state = 'open';
```

If the input has no rows, `count(*)` returns zero and other aggregate functions follow their null/empty-set rules.

## Advanced Grouping

Advanced grouping creates subtotal or multiple grouping sets when admitted.

Rollup:

```sql
select region_code,
       product_category,
       sum(total_amount) as total_amount
from app.sales_fact
group by rollup(region_code, product_category);
```

Cube:

```sql
select region_code,
       product_category,
       sum(total_amount) as total_amount
from app.sales_fact
group by cube(region_code, product_category);
```

Grouping sets:

```sql
select region_code,
       product_category,
       sum(total_amount) as total_amount
from app.sales_fact
group by grouping sets (
  (region_code, product_category),
  (region_code),
  ()
);
```

Subtotal rows must expose a descriptor that distinguishes ordinary null grouping keys from subtotal null placeholders when the query asks for that distinction.

## Grouping Id And Subtotal Indicators

When rollups, cubes, or grouping sets are admitted, subtotal rows need a way to distinguish a real `NULL` key from a subtotal placeholder. SBsql exposes that distinction through grouping metadata functions or descriptors where the active profile admits them.

```sql
select region_code,
       product_category,
       grouping_id(region_code, product_category) as grouping_level,
       sum(total_amount) as total_amount
from app.sales_fact
group by rollup(region_code, product_category)
order by grouping_level, region_code, product_category;
```

If the grouping metadata function is not admitted for a query profile, subtotal-producing grouping forms must either be refused or rendered through another explicit descriptor that preserves the same distinction.

## Multimodel Grouping

Descriptor-bound multimodel values can be grouped when the descriptor defines equality and grouping behavior.

Document field grouping:

```sql
select json_value(profile, '$.status') as status,
       count(*) as product_count
from app.product_profile
group by json_value(profile, '$.status')
having count(*) > 0;
```

Time-series grouping:

```sql
select metric_name,
       date_trunc('minute', sample_at) as sample_minute,
       avg(metric_value) as average_value
from app.metric_sample
group by metric_name, date_trunc('minute', sample_at)
order by metric_name, sample_minute;
```

Vector/search/graph evidence can contribute source rows, scores, distances, or path attributes, but final grouping input rows still require descriptor, visibility, and authorization recheck.

## Functional Dependency And Projection Rules

In an aggregate query, each projected expression must be one of:

- a grouping key;
- an aggregate expression;
- a deterministic expression over grouping keys and aggregates;
- an expression proven functionally dependent on the grouping keys by a declared key or constraint;
- a constant or parameter descriptor.

Example:

```sql
select customer_id,
       max(customer_name) as display_name,
       count(*) as order_count
from app.customer_order_projection
group by customer_id;
```

Using `max(customer_name)` makes the aggregation explicit. If a table constraint proves `customer_id` determines `customer_name`, a policy may admit direct projection; otherwise the query should be refused as ambiguous.

## Execution And Resource Rules

The engine may use hash aggregation, sort aggregation, streaming aggregation, index-assisted grouping, partial aggregation, or spill-to-disk work areas where admitted.

| Concern | Rule |
| --- | --- |
| Input visibility | Input rows must pass MGA snapshot visibility and security policy. |
| Accumulator descriptors | Aggregate state uses function-specific descriptors. |
| Memory pressure | Aggregation may spill according to resource policy. |
| Ordering | `GROUP BY` does not guarantee result order; use `ORDER BY`. |
| Index evidence | Index order can help grouping but cannot bypass row recheck. |
| Parallel/partial aggregation | Must preserve exact grouping and accumulator semantics. |
| Recovery | A failed query must not publish partial aggregate results as durable state. |

## Proof Expectations

The `GROUP BY` and `HAVING` proof suite should include:

- grouping by columns, expressions, collated text, temporal buckets, structured extracted values, and null keys;
- aggregate return-descriptor checks for count, sum, average, minimum, maximum, distinct aggregates, and collection aggregates where admitted;
- rejection of non-grouped, non-dependent projected columns;
- `WHERE` versus `HAVING` evaluation order;
- grouping sets, rollups, cubes, subtotal indicators, and empty grouping set behavior where admitted;
- spill and memory-pressure paths that preserve exact aggregate state;
- index-assisted and unordered aggregation paths proving final row recheck;
- deterministic result ordering only when `ORDER BY` is present.

## Diagnostics And Refusals

| Condition | Expected diagnostic class |
| --- | --- |
| Non-grouped source column projected | Grouping descriptor error. |
| Aggregate argument type unsupported | Function descriptor mismatch. |
| Grouping expression has no equality descriptor | Grouping unsupported. |
| `HAVING` references unavailable column | Name resolution or grouping error. |
| Advanced grouping not admitted | Unsupported surface. |
| Memory or spill policy refuses query | Resource refusal. |
| Hidden source or column | Authorization or sandbox denied. |
| Recovery-required state | Operation fenced until recovery action completes. |

## Verification Checklist

| Check | Required outcome |
| --- | --- |
| Parse | `GROUP BY` and `HAVING` shape is recognized by SBsql. |
| Bind | Grouping keys, aggregate functions, accumulator descriptors, and predicates resolve. |
| Authorize | Effective user or agent UUID may read all source expressions. |
| Admit | SBLR query route and result descriptors are accepted by the engine verifier. |
| Execute | Groups and aggregates are built from visible, authorized rows only. |
| Filter | `HAVING` filters aggregate groups after accumulator state exists. |
| Render | Result descriptors expose only authorized grouping and aggregate values. |
