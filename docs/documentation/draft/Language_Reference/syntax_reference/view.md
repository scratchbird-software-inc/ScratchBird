# View Lifecycle

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `syntax_reference_view_lifecycle`


## Purpose

Views are named, descriptor-bound query objects. A normal view stores a query definition, result descriptor, dependency graph, security mode, and metadata rendering policy. It does not store its own base row versions. A materialized view stores the same view definition plus a relation-like materialization descriptor and one or more committed materialization generations.

Both forms expose relation-shaped output to `SELECT`, joins, metadata discovery, grants, and catalog projections. Neither form gives SQL text authority over storage, transaction finality, authorization, or dependency identity.

Related reference pages:

- [Schema Tree And Name Resolution](schema_tree_and_name_resolution.md)
- [SELECT Statement](select.md)
- [WITH And Common Table Expressions](with.md)
- [Table Lifecycle](table.md)
- [Index Lifecycle](index.md)
- [Policy, Mask, And Row-Level Security Lifecycle](policy_mask_and_rls.md)
- [Trigger Lifecycle](trigger.md)
- [Procedure And Procedural SQL Reference](procedural_sql.md)

## Complete Lifecycle Model

1. Define the view with enough descriptor, dependency, security, materialization, and policy metadata for the binder and engine verifier to reason about it.
2. Bind the statement to UUID catalog identity, source-object UUIDs, output descriptors, and security context.
3. Admit the catalog mutation through SBLR and engine verification.
4. Make the view definition or materialized generation visible only when the owning transaction commits.
5. Invalidate dependent plans, parser caches, driver metadata, UDR metadata, support-bundle projections, catalog projections, and materialized-view rewrite candidates that rely on the changed object.
6. Retire or drop the view only after dependency, privilege, transaction, recovery, and sandbox checks pass.

## View Kinds

| Kind | Stores rows | Primary contract |
| --- | --- | --- |
| Normal view | No | Stores a bound query descriptor and presents it as a reusable relation shape. The executor reads source objects when the view is queried. |
| Updatable view | No | A normal view with an admitted update mapping, `WITH CHECK OPTION`, `INSTEAD OF` trigger, or SBsql rewrite rule. |
| Security-barrier view | No | A normal view whose predicate/security boundary constrains optimizer rewrites and pushdown. |
| Materialized view | Yes | Stores a query descriptor plus committed materialized result generations. Reads may use the stored generation subject to freshness, authorization, and policy checks. |
| Catalog projection view | Usually no | Renders catalog metadata in an authorized user-facing shape. Authority remains in the underlying catalog and security policy. |
| Multimodel projection view | Depends on definition | Presents document, key-value, graph, vector, search, or time-series rowset output as a named relation shape. |

The resolver treats tables, views, materialized views, foreign tables, and other admitted relation-like objects as compatible relation targets when the surrounding statement asks for a rowset source.

## Lifecycle Statement Surface

| Operation | Surface | Contract |
| --- | --- | --- |
| Create view | `CREATE VIEW` | Creates the durable view UUID, stored source reference, bound SBLR/query descriptor, dependency graph, result descriptor, and security mode. |
| Create materialized view | `CREATE MATERIALIZED VIEW` | Creates the durable materialized-view UUID, query descriptor, relation-like storage descriptor, initial materialization state, refresh policy, and dependency graph. |
| Alter view | `ALTER VIEW` | Changes admitted view metadata such as security mode, check option, barrier behavior, refresh policy, storage policy, owner, or compiled representation. |
| Refresh materialized view | `REFRESH MATERIALIZED VIEW` | Rebuilds, replaces, or advances a materialized generation through an admitted refresh route. The committed generation changes atomically under MGA. |
| Rename | `RENAME VIEW ... TO ...`, `ALTER VIEW ... RENAME TO ...`, materialized equivalents where admitted | Changes only the resolver name. Source dependencies, grants, materialized generations, and stored executable form remain bound to the object UUID. |
| Comment | `COMMENT ON VIEW ... IS ...`, `COMMENT ON MATERIALIZED VIEW ... IS ...` | Stores authorized descriptive metadata on the catalog row. |
| Show | `SHOW VIEW ...`, `SHOW VIEWS`, `SHOW MATERIALIZED VIEWS` | Returns authorized metadata, security mode, dependencies, materialization state, refresh readiness, and result descriptor projections. |
| Describe | `DESCRIBE VIEW ...`, `DESCRIBE MATERIALIZED VIEW ...` | Returns authorized result columns, descriptors, dependency graph, security mode, materialization policy, source-reference metadata, indexes, and freshness fields. |
| Recreate | `RECREATE VIEW ...`, `RECREATE MATERIALIZED VIEW ...` | Replaces the definition only through a fresh bind/lower/admit route and dependency invalidation. Existing materialized rows cannot be reused as new definition truth without admitted refresh evidence. |
| Drop | `DROP VIEW ... [RESTRICT | CASCADE]`, `DROP MATERIALIZED VIEW ... [RESTRICT | CASCADE]` | Retires the object only after dependent views, routines, grants, indexes, materialized generations, and catalog projections are handled. |

The original SQL text can be retained for authorized metadata rendering, but execution authority is the bound SBLR/query representation plus UUID-resolved dependencies.

## Normal View Contract

`CREATE VIEW` stores a reusable relation shape.

```sql
create view app.open_orders as
select order_id, customer_id, submitted_at, total_amount
from app.orders
where order_state = 'open';
```

A complete view definition records:

| Part | Required behavior |
| --- | --- |
| View UUID | Durable identity for grants, dependencies, comments, plans, and metadata projection. |
| Resolver name | User-visible name bound through the schema resolver. |
| Source query descriptor | Bound SBLR/query representation. The parser must not execute stored SQL text. |
| Result descriptor | Output column names, ordinals, types, collations, nullability, and rendering metadata. |
| Dependencies | Source tables, views, functions, domains, collations, policies, UDR packages, sequences, and other referenced objects. |
| Security mode | Invoker, definer, owner, or another SBsql mode where admitted. |
| Updatability | Whether DML through the view is refused, rewritten, checked, or routed through triggers. |
| Check option | Predicate enforcement for inserts or updates routed through an updatable view. |
| Barrier mode | Optimizer rewrite boundary for security-sensitive views. |
| Compatibility text | Original or normalized source text for authorized metadata rendering only. |

Views expose a result descriptor even when the stored query contains expressions, joins, CTEs, document functions, graph projections, or other rowset-producing surfaces.

## Materialized View Contract

A materialized view is both a view definition and a stored result relation. It has a query descriptor, output descriptor, materialized storage descriptor, refresh policy, freshness metadata, and generation history.

```sql
create materialized view app.daily_order_totals as
select cast(submitted_at as date) as order_date,
       count(*) as order_count,
       sum(total_amount) as total_amount
from app.orders
where order_state <> 'cancelled'
group by cast(submitted_at as date);
```

Materialized view metadata includes:

| Part | Required behavior |
| --- | --- |
| Materialized-view UUID | Durable identity for relation resolution, grants, refresh operations, indexes, comments, and dependencies. |
| Query descriptor | Bound query used to populate or refresh the materialized rows. |
| Result descriptor | Column descriptors visible to readers and indexes. |
| Storage descriptor | Row storage, overflow, filespace, compression/encryption policy where admitted, and cleanup behavior. |
| Population state | `populated`, `unpopulated`, `refreshing`, `stale`, `invalid`, or equivalent authorized status projection. |
| Generation identity | Committed materialization generation used for visibility, refresh, rollback, crash recovery, and diagnostics. |
| Freshness policy | Manual refresh, scheduled refresh, incremental refresh, or event-driven maintenance where admitted. |
| Source dependency epoch | Catalog, security, descriptor, and resource epochs that prove which source state the generation reflects. |
| Refresh authority | Grants, source-read authority, security mode, and policy checks required to refresh. |
| Rewrite eligibility | Whether the optimizer may use the materialized view to answer another query. |

The baseline refresh model is replacement refresh: evaluate the bound query under an admitted transaction view, build a new generation, validate descriptors and indexes, then publish that generation at commit. Readers see either the old committed generation or the new committed generation. They must not see a half-built refresh.

Incremental refresh, streaming refresh, partition refresh, and event-driven refresh are admitted only when SBsql and the engine route define the required ordering, idempotency, dependency, and crash-recovery evidence.

## Population And Refresh Semantics

`REFRESH MATERIALIZED VIEW` changes the materialized generation, not the view definition.

```sql
refresh materialized view app.daily_order_totals;
```

Refresh behavior must define:

| Concern | Contract |
| --- | --- |
| Snapshot | Source rows are read through a transactionally consistent view admitted by the refresh policy. |
| Security | Refresh uses the materialized view's security mode and the effective user or agent authority required by policy. |
| Replacement | A full refresh creates a replacement generation and commits it atomically. |
| Rollback | A failed or rolled-back refresh leaves the previous committed generation visible. |
| Crash recovery | Recovery must choose a complete old generation, a complete new generation, or a fenced recovery-required state. |
| Index maintenance | Indexes on the materialized view must be rebuilt, validated, or advanced consistently with the new generation. |
| Freshness | `SHOW` and `DESCRIBE` must expose authorized freshness state without leaking hidden source objects. |
| Unpopulated state | A materialized view created without data or left unpopulated must refuse reads with a clear diagnostic unless SBsql explicitly defines empty-result behavior for that operation. |

Refresh modifiers such as population mode, partition selection, or scheduled refresh are accepted only when SBsql defines and admits a ScratchBird refresh contract for them. Unsupported refresh forms must fail before changing catalog or storage state.

## Updatable Views And Check Options

A view is read-only unless an admitted update route exists.

| Update route | Behavior |
| --- | --- |
| Simple rewrite | DML on a simple projection is rewritten to one base table when the view definition is provably key-preserving and unambiguous. |
| `WITH CHECK OPTION` | Inserts and updates through the view must satisfy the view predicate after the change. |
| `INSTEAD OF` trigger | DML is routed through an authorized trigger body. Trigger execution remains under engine transaction authority. |
| Refusal | Joins, aggregates, grouping, set operations, window functions, materialized views, or multimodel projections are read-only unless an admitted route says otherwise. |

Example:

```sql
create view app.open_order_edit as
select order_id, customer_id, submitted_at, total_amount, order_state
from app.orders
where order_state = 'open'
with check option;
```

If an update would move a row outside the view predicate, the update must fail or route through an admitted SBsql diagnostic.

## Security Modes

View security determines whose authority is used to read source objects and evaluate protected expressions.

| Mode | Contract |
| --- | --- |
| Invoker | Source access is checked against the effective user or agent querying the view. |
| Definer or owner | Source access is checked against the authorized definer/owner context where admitted. Result projection still applies caller-visible metadata and row/column policy. |
| Security barrier | Optimizer rewrites must not move caller predicates or leaky expressions across the barrier in a way that changes protected information exposure. |

A view can expose a safe projection over objects the caller cannot query directly only when the view's security descriptor and grants explicitly allow that projection. Diagnostic and metadata surfaces must avoid leaking hidden source object names through errors, counts, timing-sensitive detail, or comments.

## Dependencies And Invalidation

Views depend on every object needed to bind, execute, render, or refresh their definition.

| Dependency | Invalidation behavior |
| --- | --- |
| Source table or view | Altering columns, descriptors, policies, or object identity can invalidate the view until it is revalidated or recreated. |
| Function, procedure, or UDR | Signature or security changes can invalidate expression descriptors and compiled forms. |
| Domain, collation, type, or cast | Descriptor changes can invalidate result columns, predicates, indexes, and refresh plans. |
| Policy or grant | Security epoch changes can invalidate cached plans and materialized-view rewrite eligibility. |
| Index or statistics | Optimizer metadata can be refreshed without changing view identity; materialized-view indexes must match the committed generation. |
| Catalog projection metadata | Catalog projections and rendered metadata forms must be invalidated when the bound descriptor changes. |

Dependency handling is fail-closed. A stale view must not silently run against a changed source descriptor if the binder cannot prove compatibility.

## Optimizer And Execution Behavior

The optimizer may inline, merge, materialize, or preserve a view boundary only when doing so preserves semantics.

| Behavior | Rule |
| --- | --- |
| View inlining | Allowed for ordinary views when dependency, security, check-option, and barrier rules permit it. |
| Predicate pushdown | Allowed only when it does not cross a security barrier or change null/missing/path behavior. |
| Materialized-view scan | Reads the committed materialized generation as a relation-like source. |
| Materialized-view rewrite | May answer a compatible query from a materialized view only when freshness, descriptor, dependency, security, and predicate-equivalence proof all pass. |
| Candidate evidence | Indexes on source tables or materialized views provide candidates only. MGA visibility, security, descriptors, and predicates still require recheck. |
| Ordering | Output order is deterministic only when the outer query has an admitted `ORDER BY` or the operation surface explicitly defines stable order. |

An `ORDER BY` inside a view definition does not by itself guarantee order for every later `SELECT` from that view unless SBsql makes that part of the result contract. Portable queries should order at the outermost query that consumes the view.

## Multimodel Views

Views can expose SQL and NoSQL rowset projections through the same relation contract.

```sql
create view app.active_product_profiles as
select product_id,
       sku,
       json_value(profile, '$.name') as product_name,
       json_value(profile, '$.status') as product_status,
       embedding
from app.product_profile
where json_value(profile, '$.status') = 'active';
```

A materialized view can cache a descriptor-bound multimodel projection:

```sql
create materialized view app.product_search_rollup as
select p.product_id,
       json_value(p.profile, '$.brand') as brand,
       l2_distance(p.embedding, vector(:reference_embedding)) as reference_distance
from app.product_profile p
where json_exists(p.profile, '$.brand');
```

Document paths, graph patterns, vector distances, search scores, time-series buckets, and key-value projections remain descriptor-bound. A materialized view may store their results, but it does not make path indexes, vector indexes, search indexes, or graph evidence final row authority.

## Practical Lifecycle Example

```sql
create view app.open_orders as
select order_id, customer_id, submitted_at, total_amount
from app.orders
where order_state = 'open';

alter view app.open_orders set security invoker;
comment on view app.open_orders is 'Visible open-order projection';
show view app.open_orders;
describe view app.open_orders;
rename view app.open_orders to open_orders_live;

create materialized view app.daily_order_totals as
select cast(submitted_at as date) as order_date,
       count(*) as order_count,
       sum(total_amount) as total_amount
from app.orders
group by cast(submitted_at as date);

refresh materialized view app.daily_order_totals;
show materialized views in schema app;
describe materialized view app.daily_order_totals;
drop materialized view app.daily_order_totals restrict;
drop view app.open_orders_live restrict;
```

## Failure Modes

| Failure | Required diagnostic behavior |
| --- | --- |
| Ambiguous view name | Refuse binding before execution and identify the ambiguity through an authorized diagnostic. |
| Hidden source object | Return not-found or not-visible according to metadata hiding policy. |
| Unsupported materialization form | Refuse before catalog mutation; do not silently create a normal view or table. |
| Stale source descriptor | Refuse execution, refresh, or rewrite until the view is revalidated or recreated. |
| Unpopulated materialized view | Refuse reads or unsupported operations with a clear unpopulated diagnostic. |
| Unsafe refresh | Refuse if the engine cannot prove snapshot, generation, index, dependency, and rollback safety. |
| Security barrier violation | Refuse an optimizer rewrite or predicate pushdown that would cross the barrier unsafely. |
| Updatable-view ambiguity | Refuse DML when the target base row or check option cannot be proven. |
| Dependency remains on drop | `RESTRICT` refuses; `CASCADE` requires explicit dependency handling and authorization. |
| Recovery fenced | Refuse catalog mutation or refresh until recovery state admits writes. |

## Boundaries

- User-visible names are resolver input; UUID rows are durable identity.
- The parser cannot create catalog truth by accepting syntax.
- Catalog DDL and materialized refresh publication must be transactionally visible and rollback-safe.
- A normal view does not own base table rows.
- A materialized view owns its materialized generation, not source-table transaction finality.
- Support and diagnostic surfaces may inspect the object only through authorized projections.

## Verification Checklist

| Check | Required Outcome |
| --- | --- |
| Parse | Statement shape is recognized by SBsql. |
| Bind | Names, UUIDs, descriptors, options, security modes, and dependencies resolve exactly. |
| Authorize | The effective user or agent UUID is allowed to create, query, refresh, alter, or drop the object. |
| Admit | SBLR route and result shape are accepted by the engine verifier. |
| Validate result descriptors | Output columns, types, nullability, collations, and multimodel descriptors are stable and explicit. |
| Validate dependencies | Source object epochs, security epochs, descriptor epochs, and UDR/function dependencies are recorded. |
| Validate materialization | Materialized rows, indexes, refresh state, generation identity, and freshness metadata are consistent. |
| Commit | Definition changes and refresh generation changes become visible only through MGA transaction finality. |
| Invalidate | Dependent caches, metadata, plans, rendered metadata, and materialized rewrite candidates are refreshed or refused. |
| Rollback | Failed or rolled-back view DDL or refresh leaves no partial descriptor, generation, index, or dependency state. |
