# WHERE And Predicates

This page is part of the SBsql Language Reference Manual. It documents row filtering, predicate binding, three-valued logic, null and missing behavior, subquery predicates, multimodel predicates, optimizer use of predicate evidence, and refusal cases.

Generation task: `syntax_reference_where`

Related pages: [SELECT Statement](select.md), [FROM And Table Expressions](from.md), [WITH And Common Table Expressions](with.md), [GROUP BY And HAVING](group_by_and_having.md), [Operators](operators.md), [Operator Type Result Matrix](operator_type_result_matrix.md), [Type System Overview](../data_types/type_system_overview.md), [Conversion Matrix](../data_types/conversion_matrix.md), [Policy, Mask, And RLS Lifecycle](policy_mask_and_rls.md), and [Refusal Vectors](refusal_vectors.md).

## Purpose

`WHERE` filters candidate rows before projection, grouping, window evaluation, ordering, and limit handling. A `WHERE` predicate is not a text filter. It is a descriptor-bound boolean expression lowered to SBLR and evaluated by the engine under transaction visibility, authorization, policy, masking, row-level security, and descriptor rules.

Example:

```sql
select order_id, customer_id, total_amount
from app.orders
where order_state = 'open'
  and submitted_at >= :start_at
  and total_amount > 0;
```

The optimizer may use predicate evidence to choose an index, search route, vector candidate route, document-path route, partition pruning route, or other access path. Candidate evidence never becomes final row authority. The executor must still recheck row visibility and predicate truth before returning a row.

## Syntax

```ebnf
where_clause ::=
    WHERE predicate ;

predicate ::=
    boolean_expression ;

boolean_expression ::=
      boolean_term
    | boolean_expression OR boolean_term
    | boolean_expression XOR boolean_term ;

boolean_term ::=
      boolean_factor
    | boolean_term AND boolean_factor ;

boolean_factor ::=
      NOT boolean_factor
    | predicate_primary ;

predicate_primary ::=
      comparison_predicate
    | null_predicate
    | distinct_predicate
    | truth_predicate
    | between_predicate
    | in_predicate
    | exists_predicate
    | quantified_comparison_predicate
    | pattern_predicate
    | fulltext_predicate
    | document_predicate
    | vector_predicate
    | graph_predicate
    | timeseries_predicate
    | "(" boolean_expression ")" ;
```

SBsql is context sensitive. `WHERE`, `AND`, `OR`, `NOT`, `BETWEEN`, `IN`, `LIKE`, `EXISTS`, and related predicate words are command words inside predicate contexts and should not be treated as globally reserved identifiers outside those contexts.

## Evaluation Position

For an ordinary `SELECT`, the logical order is:

1. bind `WITH` CTEs;
2. bind `FROM` row sources;
3. apply row visibility, authorization, and row-level security;
4. evaluate `WHERE` for each candidate row;
5. group rows and compute aggregates;
6. apply `HAVING`;
7. evaluate select-list expressions and window functions;
8. apply set operations, ordering, limit, offset, and fetch rules.

The optimizer can reorder physical work only when it proves the same visible result under descriptor, transaction, and policy rules.

## Three-Valued Logic

Predicates produce `true`, `false`, or `unknown` where SQL `NULL`, document missing values, optional fields, or protected values participate.

| Predicate Result | Row Passes `WHERE`? |
| --- | --- |
| `true` | Yes. |
| `false` | No. |
| `unknown` | No. |

Logical operators follow three-valued logic:

| Expression | Result |
| --- | --- |
| `true and unknown` | `unknown` |
| `false and unknown` | `false` |
| `true or unknown` | `true` |
| `false or unknown` | `unknown` |
| `not unknown` | `unknown` |

Use explicit null-aware predicates when null state matters:

```sql
select customer_id
from app.customer
where email_address is not null;
```

## Comparison Predicates

```ebnf
comparison_predicate ::=
    expression comparison_operator expression ;

comparison_operator ::=
      "=" | "<>" | "!=" | "<" | "<=" | ">" | ">=" ;
```

Comparisons require compatible descriptors. The binder resolves operand types, domains, collations, charsets, timezones, numeric widening, and cast rules before execution.

```sql
select order_id
from app.orders
where total_amount >= cast(:minimum_amount as decimal(18,2));
```

Text comparison uses the bound collation descriptor:

```sql
select customer_id
from app.customer
where display_name collate sys.fn.unicode_ci = :name_text;
```

Temporal comparison uses the bound temporal descriptor and timezone policy:

```sql
select event_id
from app.event_log
where occurred_at >= timestamp '2026-06-01 00:00:00';
```

## Null And Truth Predicates

```ebnf
null_predicate ::=
    expression IS NOT? NULL ;

truth_predicate ::=
    expression IS NOT? (TRUE | FALSE | UNKNOWN) ;

distinct_predicate ::=
    expression IS NOT? DISTINCT FROM expression ;
```

`IS DISTINCT FROM` and `IS NOT DISTINCT FROM` are null-aware comparison forms:

```sql
select customer_id
from app.customer
where preferred_name is distinct from legal_name;
```

Truth predicates are useful for nullable booleans:

```sql
select policy_id
from app.policy_audit
where approved is not true;
```

## BETWEEN

```ebnf
between_predicate ::=
    expression NOT? BETWEEN expression AND expression ;
```

`BETWEEN` is inclusive when admitted:

```sql
select order_id
from app.orders
where submitted_at between :start_at and :end_at;
```

The three expressions must bind to compatible descriptors. For text and temporal values, collation and timezone behavior are descriptor-owned.

`NOT BETWEEN` negates the predicate after null handling. If any required operand is null and no operation-specific null rule applies, the result is `unknown`.

## IN And NOT IN

```ebnf
in_predicate ::=
    expression NOT? IN "(" in_value_list_or_subquery ")" ;

in_value_list_or_subquery ::=
      expression ("," expression)*
    | query_dml_stmt ;
```

Value list:

```sql
select order_id
from app.orders
where order_state in ('open', 'held', 'ready');
```

Subquery:

```sql
select customer_id
from app.customer
where customer_id in (
  select customer_id
  from app.orders
  where submitted_at >= :start_at
);
```

`NOT IN` with nulls can produce `unknown`. Prefer `NOT EXISTS` when the intended logic is anti-join semantics:

```sql
select c.customer_id
from app.customer c
where not exists (
  select 1
  from app.orders o
  where o.customer_id = c.customer_id
);
```

## EXISTS

```ebnf
exists_predicate ::=
    EXISTS "(" query_dml_stmt ")" ;
```

`EXISTS` tests whether the subquery returns at least one visible row:

```sql
select c.customer_id
from app.customer c
where exists (
  select 1
  from app.orders o
  where o.customer_id = c.customer_id
    and o.order_state = 'open'
);
```

The subquery may be correlated. Correlation references bind to source descriptors from the outer query block.

## Quantified Comparison

```ebnf
quantified_comparison_predicate ::=
    expression comparison_operator (ANY | SOME | ALL) "(" query_dml_stmt ")" ;
```

Examples:

```sql
select product_id
from app.product p
where p.unit_price > all (
  select c.unit_price
  from app.competitor_price c
  where c.product_id = p.product_id
);
```

Quantified comparison uses descriptor-compatible values from the subquery. Empty-set, null, and unknown behavior is defined by the quantifier contract and comparison descriptor.

## Pattern Predicates

```ebnf
pattern_predicate ::=
      expression NOT? LIKE expression escape_clause?
    | expression NOT? ILIKE expression escape_clause?
    | regexp_like_call ;

escape_clause ::=
    ESCAPE expression ;
```

Examples:

```sql
select customer_id
from app.customer
where email_address like '%@example.test';
```

```sql
select customer_id
from app.customer
where display_name ilike :prefix || '%';
```

Pattern predicates are text operations. They bind charset, collation, escape, case-folding, and pattern-validity rules before execution.

## Document Predicates

Document predicates operate on descriptor-bound document values, not raw text authority.

```sql
select document_id
from app.order_documents
where json_exists(body, '$.customer.id')
  and json_value(body, '$.status') = 'open';
```

Missing path, JSON null, SQL `NULL`, typed scalar conversion, and path errors follow the bound JSON/document operation. A document-path index may provide candidates, but final predicate truth is rechecked.

## Vector Predicates

Vector predicates and distance expressions require matching dimensions, element descriptors, metric policy, and exact-recheck behavior.

```sql
select product_id
from app.product_embeddings
where vector_dims(embedding) = vector_dims(vector(:query_embedding))
  and l2_distance(embedding, vector(:query_embedding)) < 0.25
order by l2_distance(embedding, vector(:query_embedding)), product_id
limit 20;
```

Vector candidate sets are ranking evidence. They do not bypass row visibility, descriptor checks, or the final distance predicate.

## Search Predicates

Search predicates use a search descriptor and result policy.

```sql
select product_id, display_name
from app.products
where text_search_matches(search_document, :query_text)
order by product_id;
```

Search indexes may narrow candidates or provide ranking evidence. The executor still verifies visibility and predicate truth.

## Graph And Time-Series Predicates

Graph and time-series rowset projections can be filtered with ordinary predicates once they expose descriptor-bound columns:

```sql
select path_id, path_depth
from app.product_paths
where path_depth <= :max_depth
  and edge_kind = 'related';
```

```sql
select series_id, bucket_start, value
from app.metric_samples
where bucket_start >= :start_at
  and bucket_start < :end_at
  and value > :threshold;
```

Traversal, bucket, interpolation, and gap-fill behavior belong to the source operation. `WHERE` filters the rowset it receives.

## Predicate Pushdown And Index Use

The optimizer may push predicates into access paths, joins, derived tables, CTEs, materialized views, bridge relations, document paths, search indexes, vector indexes, and time-series partitions only when semantics are preserved.

Pushdown must not change:

- transaction snapshot visibility;
- row-level security;
- column masks;
- protected-value release rules;
- null and missing behavior;
- collation and timezone behavior;
- outer-join null extension;
- volatile function evaluation order where observable;
- error/refusal timing where the language contract requires it.

Example candidate index use:

```sql
select order_id
from app.orders
where customer_id = :customer_id
  and submitted_at >= :start_at;
```

An index on `(customer_id, submitted_at)` may reduce candidate rows. The executor still rechecks `customer_id`, `submitted_at`, row visibility, and authorization.

## Predicate Order

SBsql does not guarantee left-to-right predicate evaluation except where a function or operator contract explicitly requires it. The optimizer may reorder deterministic predicates when safe.

Do not use predicate order as a guard for unsafe expressions:

```sql
-- Prefer a safe expression contract or CASE when division must be guarded.
select order_id
from app.orders
where item_count <> 0
  and total_amount / item_count > 100;
```

If the division rule can raise on zero, use a form whose descriptor contract defines safe evaluation:

```sql
select order_id
from app.orders
where case
        when item_count = 0 then false
        else total_amount / item_count > 100
      end;
```

## DML Predicates

`WHERE` also appears in `UPDATE`, `DELETE`, `MERGE`, and some administrative rowset operations. In DML, the predicate decides candidate rows, but mutation still requires write authority and MGA conflict handling.

```sql
update app.orders
set order_state = 'closed'
where order_state = 'ready'
  and submitted_at < :cutoff;
```

```sql
delete from app.session_token
where expires_at < current_timestamp;
```

## Refusal And Diagnostics

| Condition | Result |
| --- | --- |
| Predicate expression does not bind to boolean | Bind diagnostic. |
| Operand descriptors are incompatible | Bind diagnostic or `unsupported` refusal. |
| Implicit conversion would lose information | Bind diagnostic unless explicit cast policy admits it. |
| Pattern, path, regex, vector, or search expression is invalid | Bind or runtime diagnostic according to operation contract. |
| Protected value would be released through predicate detail | `denied`. |
| Predicate references a hidden object or column | Not-visible or `denied` according to disclosure policy. |
| Predicate pushdown would change semantics | Pushdown is not admitted; query may still execute without it. |
| Required operation route is unavailable | `unsupported` or `unlicensed` according to route admission. |

## Verification Checklist

| Check | Required Outcome |
| --- | --- |
| Parse | `WHERE` clauses and predicate forms are recognized in query and DML contexts. |
| Bind | Predicates resolve to boolean descriptors with complete operand descriptors. |
| Nulls | `true`, `false`, and `unknown` behavior matches three-valued logic. |
| Subqueries | Correlated and uncorrelated subqueries bind outer references correctly. |
| Text | Collation, charset, pattern, and escape descriptors are applied. |
| Document | Missing, JSON null, SQL null, and path errors follow the document operation. |
| Vector/search | Candidate evidence is rechecked before row delivery. |
| Security | Hidden objects, masks, and RLS do not leak through predicate diagnostics. |
| Optimizer | Predicate pushdown preserves transaction, authorization, and descriptor behavior. |
| DML | Predicate-selected rows still pass write authority and MGA conflict checks. |
| Proof | Full rebuild tests regenerate parser, SBLR, optimizer, executor, security, and refusal evidence. |

## Related Surface Rows

| Surface | Kind | Family | Lowering | Result Shape |
| --- | --- | --- | --- | --- |
| `where_clause` | grammar production | query | yes | row filter |
| `predicate` | grammar production | expression | yes | boolean descriptor |
| `comparison_predicate` | grammar production | expression | yes | boolean descriptor |
| `null_predicate` | grammar production | expression | yes | boolean descriptor |
| `in_predicate` | grammar production | expression | yes | boolean descriptor |
| `exists_predicate` | grammar production | query | yes | boolean descriptor |
| `pattern_predicate` | grammar production | expression | yes | boolean descriptor |
| `document_predicate` | grammar production | multimodel | yes | boolean descriptor |
| `vector_predicate` | grammar production | multimodel | yes | boolean descriptor |
| `search_predicate` | grammar production | multimodel | yes | boolean descriptor |
