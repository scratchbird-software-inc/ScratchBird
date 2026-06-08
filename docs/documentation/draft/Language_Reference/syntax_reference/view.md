# View Lifecycle

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `syntax_reference_view_lifecycle`


## Purpose

Views are stored query definitions with a stable result shape and security context. They expose names and descriptors while preserving authority in their source objects.

## Complete Lifecycle Model

1. Define the view with enough descriptor, dependency, security, and policy metadata for the binder and engine verifier to reason about it.
2. Bind the statement to UUID catalog identity and descriptor metadata.
3. Admit the catalog mutation through SBLR and engine verification.
4. Make the mutation visible only when the owning transaction commits.
5. Invalidate dependent plans, parser caches, driver metadata, UDR metadata, support-bundle projections, and donor compatibility views that rely on the changed object.
6. Retire or drop the view only after dependency, privilege, transaction, recovery, and sandbox checks pass.

## Lifecycle Statement Surface

| Operation | Surface | Contract |
| --- | --- | --- |
| Create | `CREATE VIEW` | Creates the durable view UUID, stored source reference, bound SBLR/query descriptor, dependency graph, result descriptor, and security mode. |
| Alter | `ALTER VIEW` | Changes admitted view metadata such as security mode, check option, refresh policy, or compiled representation. |
| Rename | `RENAME VIEW ... TO ...` | Changes only the resolver name; source dependencies, grants, and stored executable form remain bound to the view UUID. |
| Comment | `COMMENT ON VIEW ... IS ...` | Stores authorized descriptive metadata on the view catalog row. |
| Show | `SHOW VIEW ...`, `SHOW VIEWS` | Returns authorized view metadata, security mode, dependencies, and readiness. |
| Describe | `DESCRIBE VIEW ...` | Returns the authorized result columns, descriptors, dependency graph, security mode, and source-reference metadata for one view. |
| Recreate | `RECREATE VIEW ...` | Replaces the view definition only through a fresh bind/lower/admit route and dependency invalidation. |
| Drop | `DROP VIEW ... [RESTRICT | CASCADE]` | Retires the view only after dependent views, routines, grants, and compatibility catalog projections are handled. |

The original SQL text can be retained for reference, but execution authority is the bound SBLR/query representation plus UUID-resolved dependencies.

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
drop view app.open_orders_live restrict;
```

## Boundaries

- User-visible names are resolver input; UUID rows are durable identity.
- The parser cannot create catalog truth by accepting syntax.
- Catalog DDL must be transactionally visible and rollback-safe.
- Donor parser variants may render donor syntax, but catalog authority remains ScratchBird catalog authority.
- Support and diagnostic surfaces may inspect the object only through authorized projections.

## Verification Checklist

| Check | Required Outcome |
| --- | --- |
| Parse | Statement shape is recognized by the active parser profile. |
| Bind | Names, UUIDs, descriptors, options, and dependencies resolve exactly. |
| Authorize | The effective user or agent UUID is allowed to mutate the object. |
| Admit | SBLR route and result shape are accepted by the engine verifier. |
| Commit | Catalog mutation becomes visible only through MGA transaction finality. |
| Invalidate | Dependent caches, metadata, plans, and projections are refreshed or refused. |
