# Trigger Lifecycle

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `syntax_reference_trigger_lifecycle`


## Purpose

Triggers bind executable behavior to table or event changes. The trigger body runs in a defined security and transaction context and must preserve row visibility, dependency, and diagnostic rules.

The procedural trigger language is documented in [procedural_sql.md](procedural_sql.md) and [procedural_sql_triggers_and_events.md](procedural_sql_triggers_and_events.md), including table triggers, event triggers, transition values, `WHEN` filters, event capture, trigger ordering, recursion, and trigger diagnostics.

## Complete Lifecycle Model

1. Define the trigger with enough descriptor, dependency, security, and policy metadata for the binder and engine verifier to reason about it.
2. Bind the statement to UUID catalog identity and descriptor metadata.
3. Admit the catalog mutation through SBLR and engine verification.
4. Make the mutation visible only when the owning transaction commits.
5. Invalidate dependent plans, parser caches, driver metadata, UDR metadata, support-bundle projections, and donor compatibility views that rely on the changed object.
6. Retire or drop the trigger only after dependency, privilege, transaction, recovery, and sandbox checks pass.

## Lifecycle Statement Surface

| Operation | Surface | Contract |
| --- | --- | --- |
| Create | `CREATE TRIGGER` | Creates the durable trigger UUID, event binding, timing, order, row/statement scope, dependency graph, security mode, source reference, and executable SBLR or UDR binding. |
| Alter | `ALTER TRIGGER` | Changes admitted trigger metadata such as active/inactive state, firing order, event policy, security mode, or compiled representation. |
| Rename | `RENAME TRIGGER ... TO ...` | Changes resolver name only; event binding, dependencies, and executable form remain UUID-bound. |
| Comment | `COMMENT ON TRIGGER ... IS ...` | Stores authorized descriptive metadata on the trigger catalog row. |
| Show | `SHOW TRIGGER ...`, `SHOW TRIGGERS` | Returns authorized trigger metadata, readiness, event bindings, and active state. |
| Describe | `DESCRIBE TRIGGER ...` | Returns event, timing, scope, relation binding, dependency, security, and execution-binding metadata for one trigger. |
| Recreate | `RECREATE TRIGGER ...` | Replaces the trigger only through fresh parse, bind, SBLR/UDR encode, and event dependency invalidation. |
| Drop | `DROP TRIGGER ... [RESTRICT | CASCADE]` | Retires the trigger and removes event firing only after dependency and transaction checks pass. |

Trigger execution occurs inside the engine-defined transaction and security context. Parser acceptance of a trigger body never grants storage or finality authority.

## Practical Lifecycle Example

```sql
create trigger app.orders_bi
before insert on app.orders
for each row
as
begin
  if new.order_id is null then
    new.order_id = gen_uuid_v7();
  end if;
end;

alter trigger app.orders_bi inactive;
comment on trigger app.orders_bi is 'Assigns missing order UUIDs before insert';
show trigger app.orders_bi;
describe trigger app.orders_bi;
rename trigger app.orders_bi to orders_before_insert_uuid;
drop trigger app.orders_before_insert_uuid;
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
