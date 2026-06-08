# UPDATE Statement

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `syntax_reference_update`

Related pages: [SELECT Statement](select.md), [WITH And Common Table Expressions](with.md), [FROM And Table Expressions](from.md), [WHERE Clause](where.md), [Table Lifecycle](table.md), [View Lifecycle](view.md), [Index Lifecycle](index.md), [Trigger Lifecycle](trigger.md), [Transaction Control](transaction_control.md), [Operators](operators.md), and [Type System Overview](../data_types/type_system_overview.md).

## Purpose

`UPDATE` changes rows by creating replacement row versions in the active transaction. It does not mutate older visible row versions in place as user authority. Older snapshots may continue to see the prior versions until MGA visibility and cleanup rules prove they are no longer reachable.

The statement can update ordinary scalar columns, structured descriptors, document fields exposed through path targets, vector-bearing columns, graph or key-value projections, and other rowset targets when the target descriptor admits the assignment. The engine verifies write authority, row visibility, write conflicts, descriptor compatibility, constraints, indexes, triggers, policies, storage, and recovery state before any change can become durable.

## Syntax

```ebnf
update_statement ::=
    UPDATE update_target
       SET assignment_list
       update_from_clause?
       where_clause?
       returning_clause? ;

update_target ::=
    qualified_name target_alias? ;

assignment_list ::=
    assignment ("," assignment)* ;

assignment ::=
      assignment_target "=" assignment_value
    | "(" assignment_target ("," assignment_target)* ")" "=" row_value_expression ;

assignment_target ::=
      identifier
    | qualified_identifier
    | path_target ;

assignment_value ::=
      expression
    | DEFAULT ;

update_from_clause ::=
    FROM table_expression ;

where_clause ::=
    WHERE predicate ;

returning_clause ::=
    RETURNING projection_list ;
```

`update_from_clause` is an admitted row-source clause for target row qualification and assignment expressions. It does not make the source row final authority for the target write. `path_target` is admitted only when the target descriptor defines structured update behavior.

## Update Target Families

| Target family | Update contract |
| --- | --- |
| Base table | Creates replacement row versions under the table descriptor. |
| Updatable view | Updates through an admitted rewrite or `INSTEAD OF` trigger route. |
| Document projection | Updates document-bearing rows or descriptor-bound document fields. |
| Key-value projection | Updates value, metadata, TTL, or version columns exposed through a rowset descriptor. |
| Graph projection | Updates graph node or edge properties only through a graph descriptor. |
| Vector projection | Updates vector-bearing rows only when dimension, element type, metric profile, and index maintenance bind successfully. |
| Time-series projection | Updates corrective event/sample rows only when policy admits mutation of the time-series descriptor. |

Targets that are read-only, hidden by sandbox policy, recovery-fenced, or missing a deterministic write route must be refused.

## Basic UPDATE

```sql
update app.orders
   set order_state = 'closed',
       closed_at = current_timestamp
 where order_id = :order_id
returning order_id, order_state, closed_at;
```

The binder resolves `app.orders`, each assignment target, every expression, the predicate, and the `RETURNING` projection. The executor identifies rows visible for update, checks conflicts, constructs replacement row versions, and runs the table's enforcement routes.

## Assignment Semantics

| Assignment form | Behavior |
| --- | --- |
| `column = expression` | Evaluates the expression and assigns it to the target column descriptor. |
| `column = DEFAULT` | Recomputes the column default or generation policy where admitted. |
| Multi-column assignment | Assigns a row value to multiple targets after descriptor and count checks. |
| Structured path assignment | Updates a descriptor-bound field or path without granting raw storage access. |
| Generated column assignment | Refused for `GENERATED ALWAYS` targets unless the specific route admits default recomputation. |
| Identity column assignment | Refused or policy-gated according to the identity descriptor. |
| Protected value assignment | Requires protected-material policy and secret-reference rules. |

Assignments are conceptually evaluated against the candidate row and statement context, then applied to a replacement row version. SBsql does not expose an assignment order that lets one assignment target see another assignment target's newly written value unless an admitted expression rule states that behavior.

## UPDATE With Source Rows

`UPDATE ... FROM` lets a target row use values from another rowset.

```sql
update app.invoice i
   set customer_name = c.customer_name,
       updated_at = current_timestamp
  from app.customer c
 where c.customer_id = i.customer_id
   and i.invoice_state = 'open';
```

The join between target and source must be deterministic for the update route. If more than one source row can drive the same target row and no deterministic rule resolves the conflict, the engine should refuse the statement rather than choose an arbitrary value.

## Common Table Expressions

`WITH` can define source rowsets for an update. Recursive CTE support is described in [WITH And Common Table Expressions](with.md).

```sql
with stale_sessions as (
  select session_id
  from app.session
  where expires_at < current_timestamp
)
update app.session s
   set session_state = 'expired'
  from stale_sessions x
 where x.session_id = s.session_id
returning s.session_id;
```

The CTE is a source expression. It does not own target visibility or transaction finality.

## Structured And Multimodel Updates

Structured data can be updated when the target descriptor defines how the update is applied.

Document field update:

```sql
update app.product_profile
   set profile.status = 'archived',
       profile.archived_at = current_timestamp
 where product_id = :product_id
returning product_id, profile.status;
```

Vector replacement:

```sql
update app.product_embedding
   set embedding = vector(:embedding_values),
       model_name = :model_name,
       embedded_at = current_timestamp
 where product_id = :product_id;
```

Time-series correction:

```sql
update app.metric_sample
   set metric_value = :corrected_value,
       corrected_at = current_timestamp
 where series_id = :series_id
   and sample_at = :sample_at;
```

Structured updates must preserve descriptor identity, missing/null rules, index maintenance, and exact recheck requirements. A path target is never a shortcut around table privileges or row policy.

## Constraints, Triggers, And Indexes

`UPDATE` can affect every enforcement surface attached to a table.

| Mechanism | Update behavior |
| --- | --- |
| `NOT NULL` | Refuses a replacement row whose final value is null. |
| Domain and type descriptor | Validates the assigned value after coercion and default handling. |
| `CHECK` | Evaluates the bound predicate against the replacement row version. |
| `UNIQUE` and primary key | Releases and reserves key entries through transaction-aware uniqueness rules. |
| Foreign key | Verifies parent or child effects for changed key columns and action policies. |
| Row policy | Applies update visibility and `WITH CHECK` style predicates where admitted. |
| Before trigger | May validate or adjust admitted replacement values before final construction. |
| After trigger | Runs after replacement construction and constraint timing rules admit the row event. |
| Index maintenance | Retires old index evidence and creates new evidence within the same transaction. |
| Search/vector/document indexes | Receive maintenance entries and still require exact row recheck at query time. |

Changing an indexed column, document path, vector value, search field, or graph property must invalidate or maintain the affected access structure before the transaction can be considered safely committed.

## Concurrency And Conflicts

An update must prove that each target row is visible and writable to the transaction performing the update. The engine may use locks, conflict markers, transaction inventory, predicate checks, and isolation rules to detect unsafe writes.

| Situation | Required behavior |
| --- | --- |
| Row not visible to the transaction | Row is not eligible for update. |
| Row changed by a concurrent transaction | Engine applies the isolation profile and either waits, retries internally, or refuses with a conflict diagnostic. |
| Target row matches multiple source rows | Refuse unless an admitted deterministic rule resolves the source. |
| Predicate becomes false after conflict recheck | Do not update that row. |
| Transaction rolls back | Replacement row versions and index evidence are non-visible and eligible for cleanup when safe. |
| Crash/restart | Recovery classifies replacement versions using durable MGA transaction inventory. |

## RETURNING

`RETURNING` projects values from replacement rows after defaults, generated expressions, allowed trigger changes, and descriptor coercions have been applied.

```sql
update app.account
   set display_name = :display_name
 where account_id = :account_id
returning account_id, display_name, updated_at;
```

`RETURNING` is a statement result. In an explicit transaction it does not prove commit finality. If the transaction later rolls back, the replacement rows are not visible to later snapshots.

## Diagnostics And Refusals

| Condition | Expected diagnostic class |
| --- | --- |
| Target not found or hidden by sandbox | Object resolution or sandbox denied. |
| Missing update privilege | Authorization denied. |
| Target is not updatable | Unsupported or incompatible surface. |
| Assignment target not found | Descriptor resolution failure. |
| Value cannot coerce to target descriptor | Type conversion refused. |
| Generated or identity assignment not admitted | Generated value or identity violation. |
| Constraint failure | Constraint violation. |
| Duplicate key after update | Unique constraint violation. |
| Foreign key action cannot be satisfied | Referential constraint violation. |
| Write conflict | Transaction conflict or lock timeout. |
| Recovery-required state | Operation fenced until recovery action completes. |

Diagnostics should identify target, column, constraint, and conflict information only where disclosure policy permits it.

## Verification Checklist

| Check | Required outcome |
| --- | --- |
| Parse | `UPDATE` statement shape is recognized by SBsql. |
| Bind | Target, assignment descriptors, source rowsets, predicate, parameters, and `RETURNING` descriptors resolve. |
| Authorize | Effective user or agent UUID may read the source and update the target. |
| Admit | SBLR route and result shape are accepted by the engine verifier. |
| Select rows | Candidate rows are visible, authorized, and conflict-checked under the transaction profile. |
| Construct | Replacement row versions apply coercions, defaults, generated values, and structured update rules. |
| Enforce | Constraints, triggers, policies, and index maintenance run through engine-owned routes. |
| Finalize | Visibility follows MGA commit or rollback finality. |
