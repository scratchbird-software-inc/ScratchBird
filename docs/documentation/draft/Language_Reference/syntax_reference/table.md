# Table Lifecycle

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `syntax_reference_table_lifecycle`


## Purpose

Tables store row versions under MGA. A table definition must describe columns, descriptors or domains, defaults, generated expressions, constraints, storage policy, visibility policy, and dependency edges. Altering a table is a catalog mutation; dropping a table retires object identity and invalidates dependent plans and metadata projections.

## Complete Lifecycle Model

1. Define the table with enough descriptor, dependency, security, and policy metadata for the binder and engine verifier to reason about it.
2. Bind the statement to UUID catalog identity and descriptor metadata.
3. Admit the catalog mutation through SBLR and engine verification.
4. Make the mutation visible only when the owning transaction commits.
5. Invalidate dependent plans, parser caches, driver metadata, UDR metadata, support-bundle projections, and donor compatibility views that rely on the changed object.
6. Retire or drop the table only after dependency, privilege, transaction, recovery, and sandbox checks pass.

## Lifecycle Statement Surface

| Operation | Surface | Contract |
| --- | --- | --- |
| Create | `CREATE TABLE` | Creates the durable table UUID, column descriptors, constraints, storage policy bindings, dependency edges, and initial metadata visibility rules. |
| Alter | `ALTER TABLE` | Mutates table metadata under the owning transaction; incompatible descriptor, dependency, or data-rewrite changes must be admitted explicitly. |
| Rename | `RENAME TABLE ... TO ...` | Changes the resolver name only. Durable identity, privileges, dependencies, and row versions remain bound to the table UUID. |
| Comment | `COMMENT ON TABLE ... IS ...` | Stores authorized descriptive metadata on the table catalog row. A `NULL` or empty-comment policy, when admitted, removes or clears that metadata without changing table identity. |
| Show | `SHOW TABLE ...`, `SHOW TABLES` | Returns authorized table metadata projections. It must not disclose hidden objects through names, counts, diagnostics, or timing-sensitive detail. |
| Describe | `DESCRIBE TABLE ...` | Returns the authorized column, descriptor, constraint, policy, storage, and dependency view for one table. |
| Recreate | `RECREATE TABLE ...` | Performs the admitted drop-and-create lifecycle as one DDL request; it must fail closed if dependencies, privileges, data retention, or recovery state make replacement unsafe. |
| Drop | `DROP TABLE ... [RESTRICT | CASCADE]` | Retires or removes the table only through dependency-aware, transactionally visible catalog mutation. `RESTRICT` refuses remaining dependents; `CASCADE` requires explicit admitted dependency handling. |

`SHOW` and `DESCRIBE` are inspection surfaces, not shortcuts around authorization. `COMMENT ON`, `RENAME`, `RECREATE`, and `DROP` are catalog mutations and therefore require the same bind, SBLR admission, security, and MGA commit rules as `CREATE` and `ALTER`.

## Practical Lifecycle Example

```sql
create table app.orders (
  order_id uuid not null,
  customer_id uuid not null,
  submitted_at timestamp with time zone not null,
  total_amount decimal(18, 2) not null,
  order_state varchar(32) not null,
  primary key (order_id)
);

alter table app.orders
  add column fulfilled_at timestamp with time zone;

comment on table app.orders is 'Orders accepted by the application workflow';

show table app.orders;
describe table app.orders;
rename table app.orders to orders_live;
recreate table app.orders_archive (
  order_id uuid not null primary key,
  archived_at timestamp with time zone not null
);
drop table app.orders_live restrict;
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
