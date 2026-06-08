# Projection List

This page is part of the SBsql Language Reference Manual. It explains the user-facing projection contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `syntax_reference_projection`

Related pages: [SELECT Statement](select.md), [FROM And Table Expressions](from.md), [WHERE Clause](where.md), [Operators](operators.md), [Operator Type Result Matrix](operator_type_result_matrix.md), [Functions](function.md), [Policy, Mask, And RLS Lifecycle](policy_mask_and_rls.md), [INSERT](insert.md), [UPDATE](update.md), [DELETE](delete.md), [MERGE And UPSERT](merge_and_upsert.md), and [Projection EBNF](ebnf/projection.md).

## Purpose

A projection defines the columns returned by a query or by a mutation statement that returns rows. Each projection item binds an expression, derives an output descriptor, applies authorization and masking rules, and assigns a client-visible output name.

Projection is not just display formatting. It is where SBsql turns expression results, source columns, aliases, wildcard expansion, aggregate values, window values, document paths, vector scores, graph values, catalog fields, and mutation output values into a typed result descriptor.

The parser can recognize a projection list. It cannot decide whether the caller may see a source column, whether a masked value can be rendered, whether a protected value can be released, or whether a mutation result is final. Those decisions belong to binding, security policy, SBLR admission, engine execution, and MGA transaction finality.

```sql
select order_id,
       customer_id,
       order_total * tax_rate as tax_amount
from app.orders
where order_state = 'open';
```

## Syntax

The compact generated production is:

```ebnf
projection_list ::= projection ("," projection)* ;
projection      ::= expression alias_clause? ;
returning_clause ::= "RETURNING" projection_list ;
```

The full SBsql projection surface is:

```ebnf
projection_list ::=
    projection_item ("," projection_item)* ;

projection_item ::=
      wildcard_projection
    | expression_projection ;

wildcard_projection ::=
      "*"
    | source_ref "." "*" ;

expression_projection ::=
    expression projection_alias? ;

projection_alias ::=
      "AS" identifier
    | identifier ;
```

`AS` is recommended for long-lived scripts because it makes the output name intentional. A bare trailing identifier is accepted where it cannot be confused with the expression itself.

## Result Descriptor

Every projection produces an output column descriptor. The descriptor records the properties that the client, executor, optimizer, security layer, and later clauses need.

| Descriptor field | Meaning |
| --- | --- |
| Column ordinal | One-based position in the result row. |
| Stable output name | Client-visible column name chosen by alias or inferred by binding. |
| Source identity | Source object UUID and column UUID when the projection is a direct source column. |
| Expression identity | Canonical expression operation and operand descriptors when the projection is computed. |
| Type descriptor | Carrier type, domain, collation, charset, timezone, nullability, precision, scale, and structured-value metadata. |
| Security state | Column privilege, row policy, mask policy, protected-material rule, and redaction state. |
| Updatability state | Whether the column can participate in updateable view or cursor operations. |
| Visibility state | Whether the column is visible, hidden, generated, system-owned, or policy-rendered. |

The result descriptor is authority. The original expression text and output alias are not durable authority after binding.

## Projection Binding

Projection binding proceeds in this order:

1. Resolve the query source scope from `FROM`, CTEs, derived tables, catalog projections, or admitted rowset-producing routes.
2. Expand wildcard items into visible columns for the effective security context.
3. Bind each expression against the source scope and expression registry.
4. Resolve function and operator overloads using operand descriptors.
5. Apply admitted implicit casts and domain rules.
6. Derive each output descriptor.
7. Resolve output aliases and generated names.
8. Apply column, row, mask, protected-material, and sandbox policy.
9. Produce a result descriptor or a canonical refusal message vector.

The binder must preserve expression order. Projection order is the result column order unless a later operation creates a new result descriptor.

## Column Projection

A direct column projection returns a source column under the caller's visibility and mask policy.

```sql
select order_id, customer_id, submitted_at
from app.orders;
```

If a column name is ambiguous, the statement is refused.

```sql
select id
from app.orders o
join app.customers c on c.customer_id = o.customer_id;
```

Use a qualifier when more than one source exposes the same name:

```sql
select o.order_id,
       c.customer_id,
       c.display_name
from app.orders o
join app.customers c on c.customer_id = o.customer_id;
```

The qualifier resolves names only. Source UUIDs and column UUIDs are the authority carried forward.

## Wildcard Projection

`*` expands to visible columns from the current rowset scope.

```sql
select *
from app.orders;
```

Qualified wildcard expands only one source.

```sql
select o.*, c.display_name
from app.orders o
join app.customers c on c.customer_id = o.customer_id;
```

Wildcard expansion happens during binding, not execution. It uses the effective security context and metadata visibility rules at bind time. Prepared statements that use wildcard projection must be invalidated or rebound when visible columns, grants, masks, row policy, schema state, or source descriptors change.

Wildcard projection does not include hidden columns, protected values, internal row identity columns, generated evidence fields, or system-maintained fields unless the source descriptor and security policy explicitly expose them.

## Expression Projection

An expression projection evaluates a scalar expression for each result row, or once for scalar projections without a row source.

```sql
select order_id,
       order_total * tax_rate as tax_amount,
       order_total + (order_total * tax_rate) as total_with_tax
from app.orders;
```

Expression projections can include:

| Expression form | Binding rule |
| --- | --- |
| Literal | Binds to a literal descriptor such as integer, decimal, text, boolean, binary, JSON, or temporal. |
| Parameter | Binds to the parameter descriptor supplied by the prepared statement or execution context. |
| Column reference | Binds to a visible source column descriptor. |
| Function call | Binds to a function overload and result descriptor. |
| Operator expression | Binds through operator overload and type-result rules. |
| Cast | Binds through conversion and domain policy. |
| Case expression | Derives a common result descriptor from admitted arms. |
| Aggregate expression | Binds over grouped input and returns one result per group. |
| Window expression | Binds over a window partition and returns one result per input row. |
| Multimodel expression | Binds structured, document, graph, vector, search, time-series, or key-value values through declared descriptors. |

Expressions that cannot derive a single admitted output descriptor are refused before execution.

## Scalar Projection Without FROM

A `SELECT` statement can evaluate scalar projections without a row source when all expressions are source-independent.

```sql
select 1 as one,
       'two' as two,
       null as empty_value,
       true as truth;
```

This route is commonly used for expression checks, function calls, session values, type conversion, XML/JSON construction, and diagnostics. It still lowers to an admitted projection operation and is still subject to security, function policy, descriptor rules, and SBLR admission.

## Aliases And Output Names

Aliases define result column names.

```sql
select customer_id as customer,
       count(*) as order_count
from app.orders
group by customer_id;
```

Alias rules:

| Rule | Contract |
| --- | --- |
| Explicit alias | Used as the stable output name where policy admits it. |
| Inferred source name | A direct column projection uses the source column's visible name. |
| Inferred expression name | A computed expression gets a generated name unless the binder has a stable contextual name. |
| Duplicate aliases | Allowed only where the result descriptor can preserve distinct ordinals; later name resolution may require qualification or ordinal selection. |
| Hidden source name | If policy hides a source name, the output may be generated or redacted. |
| Alias authority | An alias is display metadata; it does not change source identity, privileges, or expression identity. |

Use aliases for every computed expression in stable application-facing queries.

## Projection And ORDER BY

`ORDER BY` can refer to a projection alias, a visible output column, an ordinal, or a separate bound expression depending on the query shape.

```sql
select customer_id,
       sum(order_total) as lifetime_value
from app.orders
group by customer_id
order by lifetime_value desc, customer_id;
```

Projection names used by `ORDER BY` must be unambiguous. If two projected columns have the same alias, `ORDER BY alias` is ambiguous unless the binder has a policy-defined resolution rule. Ordinals avoid name ambiguity but are brittle when projection order changes.

## Projection With DISTINCT

`DISTINCT` applies to the projected result descriptor. Two rows are duplicates when every projected value compares equal under the descriptor comparison rules.

```sql
select distinct customer_id, order_state
from app.orders;
```

Changing the projection list changes distinctness. Adding a unique column usually makes every row distinct.

## Aggregate Projection

Aggregate projection turns grouped input rows into grouped result rows.

```sql
select customer_id,
       count(*) as order_count,
       sum(order_total) as total_value
from app.orders
group by customer_id
having count(*) > 0;
```

Grouped projections must obey grouping rules. A projected expression must be:

- a grouping expression;
- an aggregate expression;
- an expression derived from grouping expressions; or
- a route-admitted expression whose descriptor is constant for the group.

Ungrouped source columns that are not functionally admitted by the group descriptor are refused.

## Window Projection

Window projection computes values over a partition while preserving row-level output.

```sql
select customer_id,
       order_id,
       row_number() over (
         partition by customer_id
         order by submitted_at desc, order_id
       ) as customer_order_rank
from app.orders;
```

Window expressions have their own partition and order descriptors. The final result order is still controlled by the outer `ORDER BY` clause.

## Structured And Multimodel Projection

Structured values can be projected directly only when the output descriptor supports that value family. Otherwise, project a scalar field or declared operation result.

Document projection:

```sql
select document_id,
       json_value(body, '$.customer.id') as customer_id,
       json_value(body, '$.total') as total_text
from app.order_documents;
```

Vector projection:

```sql
select product_id,
       l2_distance(embedding, vector(:query_embedding)) as distance
from app.product_embeddings
order by distance, product_id
limit 20;
```

Search, graph, time-series, and key-value routes follow the same rule: the projected value must bind to a result descriptor and must not bypass row visibility, authorization, or policy.

## Masking, RLS, And Protected Values

Projection is where visible values are rendered for the caller. That makes it one of the main enforcement points for column grants, masks, protected material, and row-level security.

| Security layer | Projection effect |
| --- | --- |
| Table privilege | Determines whether the relation can be read at all. |
| Column privilege | Determines whether a column can appear in the result descriptor. |
| Row-level security | Filters which rows reach the projection. |
| Mask policy | Rewrites or redacts values before the client receives them. |
| Protected material policy | Controls whether raw protected values can be released or must be redacted/refused. |
| Sandbox policy | Prevents resolving or projecting objects outside the session's admitted root. |

Example:

```sql
select customer_id,
       display_name,
       email
from app.customer;
```

The result may contain the real `email`, a masked email, `NULL`, a redaction marker, or a refusal depending on the effective security context and the mask/protected-material policy. The stored row is not changed by projection masking.

## RETURNING Projection

`RETURNING` uses projection mechanics for mutation statements that expose rows after an admitted mutation route.

```sql
insert into app.orders(customer_id, order_total)
values (:customer_id, :order_total)
returning order_id, customer_id, order_total;
```

```sql
update app.orders
set order_state = 'closed'
where order_id = :order_id
returning order_id, order_state, updated_at;
```

The mutation route owns write admission, constraints, triggers, generated columns, and MGA finality. The `RETURNING` projection owns result descriptors and value rendering for the rows the mutation route is allowed to expose.

`RETURNING *` expands over the mutation output row descriptor, not over arbitrary base storage. Hidden generated fields, system fields, and protected values are still subject to policy.

## Updatable Projection Boundaries

Some projected columns can participate in updateable views, positioned updates, or client metadata. A projection is updatable only when the binder can map it back to a single mutable target descriptor and policy admits the route.

| Projection | Usually updatable? | Reason |
| --- | --- | --- |
| Direct base column | Possible | Stable source column UUID can be mapped back. |
| Masked column | Usually no | The visible value may not equal the stored value. |
| Expression | No | Computed value has no single storage target. |
| Aggregate | No | Grouped value represents many input rows. |
| Window expression | No | Computed per row but not a base storage column. |
| Literal or parameter | No | No storage target. |
| View column | Policy dependent | Must map to one admitted base column or view-owned update route. |

Updatability metadata is part of the result descriptor. It is not inferred by the client from output names.

## Diagnostics And Refusal Cases

Projection must fail closed for invalid or unsafe result construction.

| Case | Expected behavior |
| --- | --- |
| Unknown column | Refuse with a name-resolution diagnostic. |
| Ambiguous column | Refuse with an ambiguity diagnostic. |
| Ambiguous alias reuse | Refuse later alias resolution where ambiguity matters. |
| Hidden column in wildcard | Omit it from expansion or refuse according to policy. |
| Unauthorized column | Refuse or omit according to policy and statement shape. |
| Protected value without release authority | Redact or refuse according to protected-material policy. |
| Mask expression descriptor mismatch | Refuse with a descriptor diagnostic. |
| Expression overload cannot resolve | Refuse with function/operator binding diagnostic. |
| Incompatible aggregate grouping | Refuse with grouping diagnostic. |
| Unsupported structured value projection | Refuse with descriptor or route diagnostic. |
| Result descriptor cannot be built | Refuse before execution. |

Diagnostics must not leak hidden object names, protected values, raw expression text, or sandbox-external metadata unless policy explicitly admits that disclosure.

## Practical Patterns

Application-facing projection:

```sql
select order_id,
       customer_id,
       submitted_at,
       order_total
from app.orders
order by submitted_at desc, order_id
limit 50;
```

Computed projection:

```sql
select order_id,
       order_total,
       order_total * tax_rate as tax_amount,
       order_total + (order_total * tax_rate) as total_with_tax
from app.orders;
```

Catalog projection:

```sql
select object_uuid,
       object_name,
       object_kind
from sys.catalog.objects
order by object_kind, object_name;
```

Mutation-return projection:

```sql
delete from app.order_queue
where order_id = :order_id
returning order_id, removed_at;
```

## Proof Expectations

Public proof for projection should include:

- parser acceptance for projection lists, aliases, wildcard forms, scalar projections, and `RETURNING` forms;
- exact SBLR lowering for scalar projection through `query.evaluate_projection`;
- result descriptor proof for literals, parameters, source columns, functions, operators, casts, JSON/XML constructors, case expressions, aggregate outputs, and window outputs;
- server admission and engine dispatch proof for projection evaluation;
- rowset proof for `SELECT *` expansion against a descriptor-bound source;
- masking and protected-material proof that stored values are not leaked through projection;
- RLS proof that filtered rows do not reach projection;
- mutation proof that `RETURNING` exposes only rows and columns admitted by the mutation route;
- cache invalidation proof when wildcard expansion, grants, masks, source descriptors, or result names change;
- refusal proof for unknown names, ambiguous names, incompatible descriptors, unauthorized columns, and unsupported structured projections.

The visible public route evidence includes scalar projection fixtures such as `SELECT 1 AS one, 'two' AS two, NULL AS empty_value, TRUE AS truth`, expression and function projection fixtures lowered through `query.evaluate_projection`, and rowset fixtures such as `SELECT * FROM customer`. Those fixtures prove bounded parser binding, SBLR admission, server dispatch, and engine projection routing for the public surface.

## Verification Checklist

| Check | Required outcome |
| --- | --- |
| Parse | Projection list shape is recognized contextually by SBsql. |
| Bind | Source names, expression operands, aliases, wildcard expansions, and descriptors resolve exactly. |
| Authorize | Table, column, row, mask, protected-value, and sandbox policy are applied. |
| Admit | SBLR route and result shape are accepted by the engine verifier. |
| Execute | Projection values are computed or read only through admitted engine routes. |
| Describe | Result descriptor contains stable names, ordinals, types, nullability, policy state, and source identity where applicable. |
| Invalidate | Plans and metadata that depend on projection shape are refreshed or refused after changes. |
| Protect | Hidden, masked, and protected values do not leak through output, diagnostics, or support material. |
