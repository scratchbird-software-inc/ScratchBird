# MERGE And UPSERT

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `syntax_reference_merge_upsert`

Related pages: [INSERT Statement](insert.md), [UPDATE Statement](update.md), [DELETE Statement](delete.md), [SELECT Statement](select.md), [WITH And Common Table Expressions](with.md), [Table Lifecycle](table.md), [Index Lifecycle](index.md), [Trigger Lifecycle](trigger.md), [Transaction Control](transaction_control.md), and [Refusal Vectors](refusal_vectors.md).

## Purpose

`MERGE` and `UPSERT` express conditional data changes as one admitted statement. They are useful when the desired operation depends on whether a target row matches a source row or conflicts with a declared key.

`MERGE` binds a target rowset, a source rowset, a match predicate, and ordered match actions. `UPSERT` binds an insert input plus a conflict target and conflict action. Both forms require deterministic rules so the engine can preserve descriptor correctness, constraints, index maintenance, triggers, row policies, and MGA transaction authority.

## Syntax

```ebnf
merge_statement ::=
    MERGE INTO merge_target
    USING merge_source
       ON merge_search_condition
    merge_when_clause+
    returning_clause? ;

merge_target ::=
    qualified_name target_alias? ;

merge_source ::=
      table_expression
    | "(" query_statement ")" source_alias? ;

merge_when_clause ::=
      WHEN MATCHED merge_when_condition? THEN merge_matched_action
    | WHEN NOT MATCHED merge_when_condition? THEN merge_not_matched_action ;

merge_when_condition ::=
    AND predicate ;

merge_matched_action ::=
      UPDATE SET assignment_list
    | DELETE
    | DO NOTHING ;

merge_not_matched_action ::=
      INSERT insert_column_list? VALUES row_constructor
    | DO NOTHING ;
```

```ebnf
upsert_statement ::=
    UPSERT INTO upsert_target insert_column_list?
    upsert_source
    conflict_clause?
    returning_clause? ;

upsert_source ::=
      values_insert_source
    | query_insert_source
    | default_values_source ;

conflict_clause ::=
    ON CONFLICT conflict_target? conflict_action ;

conflict_target ::=
      "(" conflict_key ("," conflict_key)* ")"
    | ON CONSTRAINT identifier ;

conflict_key ::=
      identifier
    | qualified_identifier
    | expression ;

conflict_action ::=
      DO NOTHING
    | DO UPDATE SET assignment_list where_clause? ;
```

SBsql is context sensitive. The command words in this family are command words inside the statement and should not be treated as globally reserved identifiers outside this context.

## MERGE Model

`MERGE` classifies source rows against target rows.

```sql
merge into app.customer c
using app.customer_stage s
   on s.customer_id = c.customer_id
when matched and s.is_deleted = true then
  delete
when matched then
  update set
    customer_name = s.customer_name,
    updated_at = current_timestamp
when not matched then
  insert (customer_id, customer_name, created_at)
  values (s.customer_id, s.customer_name, current_timestamp)
returning c.customer_id;
```

The engine must bind all target columns, source columns, predicates, actions, and result descriptors before execution. A source row may not drive multiple incompatible actions. A target row may not be modified more than once by the same `MERGE` unless an explicit admitted rule makes that behavior deterministic.

## MERGE Action Rules

| Clause | Behavior |
| --- | --- |
| `WHEN MATCHED THEN UPDATE` | Creates replacement row versions for matched target rows. |
| `WHEN MATCHED THEN DELETE` | Retires matched target rows by transaction visibility rules. |
| `WHEN MATCHED THEN DO NOTHING` | Leaves matched target rows unchanged. |
| `WHEN NOT MATCHED THEN INSERT` | Constructs new target row versions from source expressions. |
| `WHEN NOT MATCHED THEN DO NOTHING` | Ignores source rows with no target match. |
| Clause predicate | Further qualifies the matched or not-matched branch. |
| Clause order | Evaluated in statement order where more than one clause could apply. |

Each action uses the same engine-owned enforcement as the corresponding `INSERT`, `UPDATE`, or `DELETE` statement.

## UPSERT Model

`UPSERT` attempts an insert and applies conflict handling when a declared conflict target detects an existing row.

```sql
upsert into app.customer (
  customer_id,
  customer_name,
  updated_at
)
values (
  :customer_id,
  :customer_name,
  current_timestamp
)
on conflict (customer_id) do update set
  customer_name = :customer_name,
  updated_at = current_timestamp
returning customer_id, customer_name, updated_at;
```

The conflict target must bind to a unique or otherwise admitted conflict descriptor. The conflict descriptor is catalog authority; the text spelling of a key column or constraint name is only resolver input.

## Conflict Actions

| Action | Contract |
| --- | --- |
| `DO NOTHING` | If a conflict is detected, no target row is changed. |
| `DO UPDATE SET ...` | If a conflict is detected, the existing target row is updated through the ordinary update route. |
| Conflict `WHERE` | Qualifies the conflict update. If false, the conflict row is left unchanged. |
| `RETURNING` | Projects inserted or updated rows, and may omit rows skipped by `DO NOTHING` according to the admitted result rule. |

Example:

```sql
upsert into app.session_cache (
  cache_key,
  cache_value,
  expires_at
)
values (
  :cache_key,
  :cache_value,
  :expires_at
)
on conflict (cache_key) do nothing;
```

## Determinism Requirements

`MERGE` and `UPSERT` must avoid ambiguous writes.

| Requirement | Reason |
| --- | --- |
| Source row shape is fixed | Target actions need known descriptors and parameters. |
| Match predicate is bound | Text comparison is not enough to identify row authority. |
| Conflict target is catalog-bound | Uniqueness and conflict detection depend on durable descriptors. |
| One target row has one action outcome | Multiple incompatible actions would make row finality ambiguous. |
| Approximate evidence is rechecked | Vector, search, graph, document, and index candidates cannot decide final row membership. |
| Concurrent conflicts are rechecked | A conflict detected before a wait or retry may no longer be valid. |

If determinism cannot be proven, the statement must return a diagnostic instead of choosing an arbitrary action.

## Multimodel Conditional Writes

Conditional writes can operate on multimodel rowsets when the target exposes descriptor-bound rows.

Document-bearing upsert:

```sql
upsert into app.document_store (
  document_key,
  document_body,
  updated_at
)
values (
  :document_key,
  json(:document_body),
  current_timestamp
)
on conflict (document_key) do update set
  document_body = json(:document_body),
  updated_at = current_timestamp;
```

Vector embedding upsert:

```sql
upsert into app.product_embedding (
  product_id,
  embedding,
  embedded_at
)
values (
  :product_id,
  vector(:embedding_values),
  current_timestamp
)
on conflict (product_id) do update set
  embedding = vector(:embedding_values),
  embedded_at = current_timestamp;
```

Graph edge merge through a rowset projection:

```sql
merge into app.graph_edge e
using app.graph_edge_stage s
   on s.edge_id = e.edge_id
when matched then
  update set edge_properties = s.edge_properties
when not matched then
  insert (edge_id, from_node_id, to_node_id, edge_properties)
  values (s.edge_id, s.from_node_id, s.to_node_id, s.edge_properties);
```

Structured values still require descriptor checks, row policy, exact predicate recheck, and index maintenance.

## Constraints, Triggers, And Indexes

`MERGE` and `UPSERT` may perform inserts, updates, deletes, or no-op actions. Enforcement follows the selected action.

| Mechanism | Conditional write behavior |
| --- | --- |
| `NOT NULL`, domain, and check constraints | Validate the final row version produced by each action. |
| Unique constraints | Detect conflicts and enforce final uniqueness under transaction rules. |
| Foreign keys | Apply parent/child checks and action timing for inserted, updated, or deleted keys. |
| Row policy | Applies read, insert, update, delete, and check predicates according to the selected action. |
| Triggers | Runs action-specific triggers. A `MERGE` may fire insert, update, or delete triggers for different rows. |
| Index maintenance | Maintains index evidence for every inserted, replaced, or retired row version. |
| Approximate indexes | Supply candidates only; exact row recheck remains mandatory. |

## Transaction And Visibility Rules

All actions in one `MERGE` or `UPSERT` statement execute in the active transaction.

| Outcome | Visibility |
| --- | --- |
| Insert action | Creates new row versions visible after commit. |
| Update action | Creates replacement row versions; older snapshots may continue to see prior versions. |
| Delete action | Retires target row versions; cleanup happens later when safe. |
| No-op action | Leaves target rows unchanged. |
| Rollback | All statement-created row versions and index evidence become non-visible. |
| Crash/restart | Recovery uses durable MGA transaction inventory to classify each row version. |

`RETURNING` reports statement results. It is not commit proof for an explicit transaction.

## Diagnostics And Refusals

| Condition | Expected diagnostic class |
| --- | --- |
| Target or source hidden by sandbox | Object resolution or sandbox denied. |
| Missing read/write privilege | Authorization denied. |
| Target is not writable | Unsupported or incompatible surface. |
| Source shape does not match action | Descriptor mismatch. |
| Multiple source rows match one target row ambiguously | Non-deterministic conditional write. |
| Conflict target is missing or not unique | Conflict descriptor failure. |
| Concurrent write changes target eligibility | Transaction conflict or retry refusal. |
| Constraint, trigger, or policy failure | Action-specific violation. |
| Recovery-required state | Operation fenced until recovery action completes. |

## Verification Checklist

| Check | Required outcome |
| --- | --- |
| Parse | `MERGE` or `UPSERT` statement shape is recognized by SBsql. |
| Bind | Target, source, conflict descriptors, predicates, action descriptors, and result descriptors resolve. |
| Authorize | Effective user or agent UUID may read sources and perform every possible target action. |
| Admit | SBLR route and result shape are accepted by the engine verifier. |
| Classify | Match and conflict detection are deterministic and transaction-aware. |
| Execute | Each selected action uses the corresponding engine-owned insert, update, or delete route. |
| Enforce | Constraints, triggers, policies, and index maintenance run for each affected row. |
| Finalize | Visibility follows MGA commit or rollback finality. |
