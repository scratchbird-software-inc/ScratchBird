# DELETE Statement

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `syntax_reference_delete`

Related pages: [SELECT Statement](select.md), [WITH And Common Table Expressions](with.md), [FROM And Table Expressions](from.md), [WHERE Clause](where.md), [Table Lifecycle](table.md), [View Lifecycle](view.md), [Index Lifecycle](index.md), [Trigger Lifecycle](trigger.md), [Transaction Control](transaction_control.md), and [Refusal Vectors](refusal_vectors.md).

## Purpose

`DELETE` retires target row versions in the active transaction. It does not immediately erase storage as parser authority. Older valid snapshots may still see prior row versions until MGA cleanup rules prove those versions are unreachable and physical reclamation is safe.

Use `DELETE` when the target is a rowset target: a table, an updatable view, or a descriptor-bound projection that admits row deletion. Use dedicated multimodel statements when the operation is primarily a document, graph, vector, search, time-series, or key-value command rather than a rowset deletion.

## Syntax

```ebnf
delete_statement ::=
    DELETE FROM delete_target
    delete_using_clause?
    where_clause?
    returning_clause? ;

delete_target ::=
    qualified_name target_alias? ;

delete_using_clause ::=
    USING table_expression ;

where_clause ::=
    WHERE predicate ;

returning_clause ::=
    RETURNING projection_list ;
```

`delete_using_clause` supplies additional rowsets for target qualification. It does not give the source rowsets authority to delete target rows.

## Delete Target Families

| Target family | Delete contract |
| --- | --- |
| Base table | Retires ordinary row versions under the table descriptor. |
| Updatable view | Deletes through an admitted rewrite or `INSTEAD OF` trigger route. |
| Document projection | Deletes document-bearing rows or descriptor-bound document records. |
| Key-value projection | Deletes key/value rows through a descriptor-bound key route. |
| Graph projection | Deletes node or edge rows only through a graph descriptor and dependency policy. |
| Vector projection | Deletes vector-bearing rows and maintains vector index evidence. |
| Time-series projection | Deletes event/sample rows only when retention and mutation policy admit it. |

Targets that are read-only, hidden by sandbox policy, recovery-fenced, or missing a deterministic delete route must be refused.

## Basic DELETE

```sql
delete from app.session_token
 where expires_at < current_timestamp
returning token_id, expires_at;
```

The binder resolves the target, predicate, parameters, and `RETURNING` projection. The executor identifies visible rows eligible for deletion, checks conflicts, records delete/replacement state, maintains indexes, fires triggers, and leaves physical cleanup to the engine.

## DELETE With Source Rows

`DELETE ... USING` lets a statement qualify target rows using another rowset.

```sql
delete from app.order_queue q
using app.closed_order c
where c.order_id = q.order_id
  and c.closed_at < :closed_before
returning q.queue_id;
```

The source rowset is read through the statement snapshot. It can qualify target rows but cannot bypass target privileges, row policy, visibility, or conflict checks.

## Common Table Expressions

`WITH` can define target qualifiers for a delete.

```sql
with expired_accounts as (
  select account_id
  from app.account
  where account_state = 'expired'
)
delete from app.account_notification n
using expired_accounts x
where x.account_id = n.account_id;
```

Recursive CTE support is described in [WITH And Common Table Expressions](with.md). A CTE is a rowset source, not transaction authority.

## Multimodel Row Deletion

`DELETE` can operate on multimodel rowsets when the target exposes descriptor-bound rows.

Document row deletion:

```sql
delete from app.document_store
 where document_key = :document_key
returning document_key;
```

Vector-bearing row deletion:

```sql
delete from app.product_embedding
 where product_id = :product_id
returning product_id, embedded_at;
```

Graph edge deletion through a rowset projection:

```sql
delete from app.graph_edge
 where edge_id = :edge_id
returning edge_id, from_node_id, to_node_id;
```

Key-value projection deletion:

```sql
delete from app.session_cache
 where cache_key = :cache_key;
```

Structured deletion must preserve descriptor rules, dependency policy, exact recheck requirements, and index maintenance. For example, deleting a graph node may be refused when edge dependencies remain unless the graph descriptor admits a cascading action.

## DELETE Without WHERE

A `DELETE` without `WHERE` targets every row visible and writable through the target descriptor.

```sql
delete from app.import_stage;
```

This is a valid statement shape, but the engine may require additional policy for large, protected, audited, or retention-controlled targets. A policy may require explicit confirmation, a stronger privilege, or an administrative route for certain targets.

## Constraints, Triggers, And Indexes

`DELETE` participates in the full enforcement model.

| Mechanism | Delete behavior |
| --- | --- |
| Foreign key | Applies `RESTRICT`, `NO ACTION`, `CASCADE`, `SET NULL`, or `SET DEFAULT` behavior where admitted by the constraint descriptor. |
| Row policy | Applies delete eligibility and visibility predicates. |
| Before trigger | May validate, refuse, or perform admitted side effects before row retirement. |
| After trigger | Runs after delete state is recorded according to trigger timing rules. |
| Index maintenance | Retires index evidence for deleted row versions within the transaction. |
| Search/vector/document indexes | Retire or invalidate candidate evidence and still require exact row recheck at query time. |
| Retention policy | May refuse deletion or transform it into a policy-defined retained state. |

Deleting a row is not the same as dropping a table, truncating a table, repairing storage, or reclaiming pages. Those are separate administrative or lifecycle surfaces.

## Transaction And Visibility Rules

`DELETE` records a transaction-owned retirement of each target row version.

| State | Visibility |
| --- | --- |
| Active deleting transaction | The deleting transaction sees its own delete according to its isolation profile. |
| Concurrent older snapshot | May continue to see the pre-delete row version. |
| Later snapshot after commit | Does not see the deleted row unless a temporal, audit, or retained-history surface exposes it. |
| Rollback succeeds | Delete markers are non-visible and the prior row versions remain visible according to snapshot rules. |
| Cleanup horizon advances | Retired row and overflow versions may be reclaimed only when no valid snapshot can reach them. |
| Crash/restart | Recovery uses durable MGA transaction inventory to classify delete state. |

Physical reclamation is engine-owned. A successful `DELETE` result should not be interpreted as immediate page reuse or file shrinkage.

## RETURNING

`RETURNING` projects values from rows selected for deletion.

```sql
delete from app.api_token
 where revoked_at is not null
returning token_id, revoked_at;
```

The projected values come from the row version being retired and any allowed statement expressions. `RETURNING` is not commit proof in an explicit transaction.

## Diagnostics And Refusals

| Condition | Expected diagnostic class |
| --- | --- |
| Target hidden by sandbox | Object resolution or sandbox denied. |
| Missing delete privilege | Authorization denied. |
| Target is not deletable | Unsupported or incompatible surface. |
| Predicate cannot bind | Descriptor or expression binding failure. |
| Foreign key restricts delete | Referential constraint violation. |
| Retention or protected-data policy refuses delete | Policy denied. |
| Write conflict | Transaction conflict or lock timeout. |
| Delete would violate graph or structured dependency policy | Dependency violation. |
| Recovery-required state | Operation fenced until recovery action completes. |

Diagnostics should identify target, constraint, dependency, and conflict information only where disclosure policy permits it.

## Verification Checklist

| Check | Required outcome |
| --- | --- |
| Parse | `DELETE` statement shape is recognized by SBsql. |
| Bind | Target, source rowsets, predicate, parameters, and `RETURNING` descriptors resolve. |
| Authorize | Effective user or agent UUID may read qualifiers and delete target rows. |
| Admit | SBLR route and result shape are accepted by the engine verifier. |
| Select rows | Candidate rows are visible, writable, and conflict-checked under the transaction profile. |
| Retire | Delete state is recorded without treating parser text as storage authority. |
| Enforce | Foreign keys, triggers, policies, dependencies, and index maintenance run through engine-owned routes. |
| Finalize | Visibility follows MGA commit or rollback finality; cleanup remains engine-owned. |
