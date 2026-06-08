# WITH And Common Table Expressions

This page is part of the SBsql Language Reference Manual. It documents ordinary and recursive common table expressions, CTE scope, column aliases, dependency order, materialization behavior, query integration, recursive fixed-point execution, cycle safety, and refusal cases.

Generation task: `syntax_reference_with`

Related pages: [SELECT Statement](select.md), [FROM And Table Expressions](from.md), [WHERE Clause](where.md), [GROUP BY And HAVING](group_by_and_having.md), [Window Clause And Window Functions](window.md), [Projection](projection.md), [ORDER BY, LIMIT, OFFSET, And FETCH](order_by_limit_offset.md), [Type System Overview](../data_types/type_system_overview.md), [Operators](operators.md), and [Refusal Vectors](refusal_vectors.md).

## Purpose

`WITH` introduces common table expressions, or CTEs, for one statement. A CTE is a named rowset expression. It can make a query easier to read, isolate a reusable subquery, stage transformations, provide statement-local names, or express recursion.

A CTE name is statement-local resolver state. It is not a durable catalog object, does not create a schema entry, and cannot grant authority over the row sources inside the CTE. Every source referenced by a CTE still binds through ordinary catalog, descriptor, transaction, and security rules.

Example:

```sql
with open_orders as (
  select order_id, customer_id, total_amount
  from app.orders
  where order_state = 'open'
)
select customer_id, sum(total_amount) as open_total
from open_orders
group by customer_id;
```

## Syntax

```ebnf
with_clause ::=
    WITH recursive_modifier? cte_list ;

recursive_modifier ::=
    RECURSIVE ;

cte_list ::=
    cte ("," cte)* ;

cte ::=
    identifier column_alias_list? cte_materialization_hint?
    AS "(" query_statement ")" ;

column_alias_list ::=
    "(" identifier ("," identifier)* ")" ;

cte_materialization_hint ::=
      MATERIALIZED
    | NOT MATERIALIZED ;
```

`WITH` attaches to a following statement that admits a query expression, such as `SELECT` and statement forms that consume a rowset. The usual form is:

```ebnf
with_query_statement ::=
    with_clause query_statement ;
```

SBsql is context sensitive. `WITH`, `RECURSIVE`, `MATERIALIZED`, and related words are command words in CTE contexts and should not be treated as globally reserved identifiers elsewhere.

## Scope

A CTE name is visible to the statement that immediately follows the `WITH` clause. It is also visible to later CTEs in the same `WITH` list according to dependency order.

```sql
with
  recent_orders as (
    select order_id, customer_id
    from app.orders
    where submitted_at >= :start_at
  ),
  recent_customers as (
    select distinct customer_id
    from recent_orders
  )
select customer_id
from recent_customers;
```

Rules:

- CTE names are scoped to one statement.
- A CTE can shadow a catalog object name inside its query block.
- Duplicate CTE names in the same `WITH` list are refused.
- A non-recursive CTE cannot refer to itself.
- A recursive CTE can refer to itself only in the recursive member.
- Nested query blocks may define their own CTE names.

## Column Aliases And Result Descriptors

Column aliases rename the CTE output descriptor:

```sql
with order_totals(customer_id, total_amount) as (
  select customer_id, sum(total_amount)
  from app.orders
  group by customer_id
)
select customer_id, total_amount
from order_totals;
```

The alias count must match the CTE result arity. If aliases are omitted, column names come from the CTE query's projection descriptors.

The result descriptor includes:

- column names;
- column order;
- descriptor UUIDs;
- nullability evidence;
- collation and charset evidence;
- domain stack evidence where preserved;
- security and redaction metadata;
- dependency and invalidation evidence.

## Ordinary CTEs

Ordinary CTEs are named subqueries:

```sql
with high_value_orders as (
  select order_id, customer_id, total_amount
  from app.orders
  where total_amount >= :minimum_total
)
select customer_id, count(*) as order_count
from high_value_orders
group by customer_id;
```

They can be referenced more than once:

```sql
with order_totals as (
  select customer_id, sum(total_amount) as total_amount
  from app.orders
  group by customer_id
)
select a.customer_id,
       a.total_amount,
       b.total_amount as peer_total
from order_totals a
join order_totals b on b.total_amount > a.total_amount;
```

The optimizer may materialize a CTE, inline it, share it, or re-evaluate it only when the selected strategy preserves descriptor, volatility, transaction, and security semantics.

## Materialization Hints

`MATERIALIZED` requests a statement-local materialized rowset. `NOT MATERIALIZED` requests inlining or re-planning as if the CTE were a derived query. They are hints unless the active route makes one behavior mandatory.

```sql
with expensive_orders materialized as (
  select order_id, customer_id, total_amount
  from app.orders
  where total_amount >= :minimum_total
)
select count(*)
from expensive_orders;
```

Materialization rules:

- materialized CTE rows are statement-local;
- materialization does not create a catalog table;
- materialized rows use the statement snapshot and authorization context;
- row-level security and masks are applied before visible CTE rows are consumed;
- volatile expressions may force materialization or prevent sharing;
- memory and spill limits are policy-controlled.

If the engine cannot honor a required materialization shape, it returns a refusal vector rather than silently changing semantics.

## Recursive CTEs

`WITH RECURSIVE` defines a CTE whose recursive member can refer to the CTE being defined. Recursive CTEs are evaluated as a fixed-point computation.

```ebnf
recursive_cte ::=
    identifier column_alias_list?
    AS "(" anchor_member recursive_set_operator recursive_member ")" ;

anchor_member ::=
    query_statement ;

recursive_set_operator ::=
      UNION
    | UNION ALL
    | UNION DISTINCT ;

recursive_member ::=
    query_statement ;
```

Basic numeric recursion:

```sql
with recursive n(value) as (
  select 1
  union all
  select value + 1
  from n
  where value < 10
)
select value
from n
order by value;
```

Hierarchy traversal:

```sql
with recursive category_tree(category_id, parent_category_id, depth) as (
  select category_id, parent_category_id, 0
  from app.category
  where parent_category_id is null

  union all

  select c.category_id, c.parent_category_id, category_tree.depth + 1
  from app.category c
  join category_tree on c.parent_category_id = category_tree.category_id
  where category_tree.depth < :max_depth
)
select category_id, parent_category_id, depth
from category_tree
order by depth, category_id;
```

The anchor member produces the initial rowset. The recursive member is repeatedly evaluated using rows produced by earlier iterations until no new admitted rows are produced, a limit is reached, or a diagnostic/refusal stops execution.

## Recursive Column And Type Rules

The anchor member and recursive member must have the same arity. Each column position must resolve to a common descriptor that can represent values from both members.

```sql
with recursive path_nodes(node_id, depth, path_text) as (
  select root_id, 0, cast(root_id as varchar(80))
  from app.graph_roots

  union all

  select e.child_id,
         path_nodes.depth + 1,
         path_nodes.path_text || '/' || cast(e.child_id as varchar(80))
  from app.graph_edge e
  join path_nodes on e.parent_id = path_nodes.node_id
  where path_nodes.depth < 20
)
select node_id, depth, path_text
from path_nodes;
```

Rules:

- alias count must match output arity;
- recursive references must bind to the CTE output descriptor;
- recursive member descriptors must be assignment-compatible with the CTE descriptor;
- nullability and domain preservation must be explicit in the common descriptor;
- collation, charset, timezone, and numeric precision must not be ambiguous;
- unsupported common-descriptor inference is refused before execution.

## Recursion Termination And Safety

Recursive CTEs need a termination path. Termination may occur because:

- the recursive member produces no new rows;
- a `WHERE` predicate limits depth or range;
- `UNION DISTINCT` removes already-seen rows until a fixed point is reached;
- a statement limit, recursion depth limit, memory limit, timeout, or cancellation policy stops execution.

Example with an explicit depth guard:

```sql
with recursive chain(node_id, depth) as (
  select :root_node_id, 0
  union all
  select e.child_id, chain.depth + 1
  from app.edge e
  join chain on e.parent_id = chain.node_id
  where chain.depth < 100
)
select node_id, depth
from chain;
```

If recursion exceeds an admitted safety limit, SBsql returns a diagnostic or refusal according to the route policy. It must not run indefinitely without cancellation and resource accounting.

## UNION ALL Versus UNION DISTINCT

`UNION ALL` preserves duplicates. It is useful for paths, depth counting, and cases where duplicate rows are meaningful.

`UNION` and `UNION DISTINCT` remove duplicate rows according to the CTE descriptor's equality and collation rules. They can help a recursive CTE reach a fixed point when duplicates would otherwise repeat.

```sql
with recursive reachable(node_id) as (
  select :root_node_id
  union
  select e.child_id
  from app.edge e
  join reachable on e.parent_id = reachable.node_id
)
select node_id
from reachable;
```

Duplicate removal requires hashable or comparable descriptors for all CTE columns. If the descriptors cannot support duplicate detection, the query is refused.

## Cycle Handling

A recursive CTE can prevent cycles with explicit state:

```sql
with recursive walk(node_id, depth, path_text) as (
  select :root_node_id, 0, cast(:root_node_id as varchar(200))
  union all
  select e.child_id,
         walk.depth + 1,
         walk.path_text || '/' || cast(e.child_id as varchar(200))
  from app.edge e
  join walk on e.parent_id = walk.node_id
  where walk.depth < 100
    and position(cast(e.child_id as varchar(200)) in walk.path_text) = 0
)
select node_id, depth
from walk;
```

When SBsql admits a dedicated cycle-detection clause or function, that surface must bind to descriptor-owned identity and equality rules. Without such a surface, explicit predicates and safety limits are required.

## CTEs In DML

A CTE can feed a data-changing statement where the statement family admits a query source:

```sql
with expired_tokens as (
  select token_id
  from app.session_token
  where expires_at < current_timestamp
)
delete from app.session_token
where token_id in (
  select token_id
  from expired_tokens
);
```

The CTE itself does not grant write authority. The target mutation still requires ordinary DML privileges, row visibility, policy admission, and MGA conflict handling.

## Multimodel CTEs

A CTE can hold any admitted rowset descriptor, including document, graph, vector, search, time-series, or key-value projections:

```sql
with ranked_matches as (
  select product_id,
         l2_distance(embedding, vector(:query_embedding)) as distance
  from app.product_embedding
  where vector_dims(embedding) = vector_dims(vector(:query_embedding))
)
select product_id, distance
from ranked_matches
where distance < :max_distance
order by distance, product_id
limit 20;
```

The CTE stores rowset descriptors, not direct access to underlying storage internals.

## Name Resolution And Shadowing

CTE names participate in statement-local name resolution. In a query block, a visible CTE name can shadow a catalog object name with the same unqualified label.

```sql
with orders as (
  select order_id
  from app.orders
)
select order_id
from orders;
```

Use qualified names when a catalog object must be referenced explicitly:

```sql
select order_id
from app.orders;
```

Resolver behavior must be deterministic. Ambiguous CTE references, duplicate aliases, hidden catalog names, and sandbox escapes must fail closed.

## Optimizer And Execution

The optimizer may:

- inline an ordinary CTE;
- materialize an ordinary CTE;
- share a materialized CTE across references;
- push safe predicates into a CTE;
- reorder joins around an inlined CTE;
- use indexes inside CTE members;
- spill materialized CTE rows when policy admits it.

The optimizer must not:

- duplicate volatile or side-effecting work when that changes semantics;
- push predicates through a CTE in a way that changes null, missing, or outer-join behavior;
- bypass RLS, masks, or protected-value policy;
- treat a CTE name as durable catalog identity;
- change recursive fixed-point behavior;
- use candidate evidence as final row authority.

## Refusal And Diagnostics

| Condition | Result |
| --- | --- |
| Duplicate CTE name in one `WITH` list | Bind diagnostic. |
| Alias count does not match row arity | Bind diagnostic. |
| Non-recursive CTE refers to itself | Bind diagnostic. |
| Recursive CTE lacks `WITH RECURSIVE` | Bind diagnostic. |
| Recursive member arity differs from anchor | Bind diagnostic. |
| Common descriptor cannot be inferred safely | Bind diagnostic or `unsupported`. |
| Recursive duplicate detection lacks comparable/hashable descriptors | `unsupported`. |
| Recursion exceeds depth, memory, timeout, or row limit | Diagnostic or `denied` according to policy. |
| Materialization hint cannot be honored when mandatory | `unsupported` or `denied`. |
| CTE references hidden or unauthorized objects | `denied` or not-visible rendering. |
| Product profile omits a required route | `unsupported` or `unlicensed` according to route admission. |

## Practical Examples

Staged filtering:

```sql
with eligible_orders as (
  select order_id, customer_id, total_amount
  from app.orders
  where order_state = 'closed'
    and total_amount > 0
)
select customer_id, sum(total_amount) as closed_total
from eligible_orders
group by customer_id
having sum(total_amount) >= :minimum_total;
```

Top rows per group:

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
where rn <= 5
order by customer_id, rn;
```

Recursive hierarchy:

```sql
with recursive category_tree(category_id, depth) as (
  select category_id, 0
  from app.category
  where parent_category_id is null
  union all
  select c.category_id, category_tree.depth + 1
  from app.category c
  join category_tree on c.parent_category_id = category_tree.category_id
  where category_tree.depth < 50
)
select category_id, depth
from category_tree
order by depth, category_id;
```

## Verification Checklist

| Check | Required Outcome |
| --- | --- |
| Parse | Ordinary and recursive CTE syntax is recognized. |
| Scope | CTE names are statement-local and resolve deterministically. |
| Aliases | Column alias lists match arity and become output descriptor names. |
| Dependencies | Later CTEs can reference earlier CTEs; invalid cycles fail closed. |
| Materialization | Inlining, materialization, sharing, and spill preserve semantics. |
| Recursion | Anchor and recursive members bind to compatible descriptors. |
| Fixed point | Recursive execution terminates through empty delta, distinct fixed point, or admitted safety limit. |
| Security | CTEs do not bypass object privileges, RLS, masks, or sandbox roots. |
| DML | CTE-fed mutations still require DML authority and MGA checks. |
| Optimizer | Predicate pushdown and join reordering preserve null, descriptor, and policy behavior. |
| Proof | Full rebuild tests regenerate parser, SBLR, optimizer, executor, recursive, security, and refusal evidence. |

## Related Surface Rows

| Surface | Kind | Family | Lowering | Result Shape |
| --- | --- | --- | --- | --- |
| `with_clause` | grammar production | query | yes | rowset descriptor |
| `cte_list` | grammar production | query | yes | CTE descriptor set |
| `cte` | grammar production | query | yes | CTE rowset descriptor |
| `recursive_cte` | grammar production | query | yes | recursive rowset descriptor |
| `anchor_member` | query member | query | yes | rowset descriptor |
| `recursive_member` | query member | query | yes | rowset descriptor |
| `column_alias_list` | grammar production | query | yes | output descriptor names |
| `materialized_cte` | execution strategy | query | yes | statement-local rowset |
| `not_materialized_cte` | execution strategy | query | yes | inlined rowset plan |
