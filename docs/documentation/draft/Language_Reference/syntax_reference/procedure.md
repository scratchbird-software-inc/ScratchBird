# Procedure Lifecycle

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `syntax_reference_procedure_lifecycle`


## Purpose

Procedures are executable catalog objects that may return rows, message vectors, or policy-admitted side effects. Procedure bodies must be stored as executable representation plus original reference text, not as unbound text only.

The procedural body language is documented in [procedural_sql.md](procedural_sql.md), with detailed coverage for [blocks and declarations](procedural_sql_blocks.md), [control flow](procedural_sql_control_flow.md), [cursors](procedural_sql_cursors.md), and [exceptions and diagnostics](procedural_sql_exceptions.md).

## Complete Lifecycle Model

1. Define the procedure with enough descriptor, dependency, security, and policy metadata for the binder and engine verifier to reason about it.
2. Bind the statement to UUID catalog identity and descriptor metadata.
3. Admit the catalog mutation through SBLR and engine verification.
4. Make the mutation visible only when the owning transaction commits.
5. Invalidate dependent plans, parser caches, driver metadata, UDR metadata, support-bundle projections, and metadata rendering views that rely on the changed object.
6. Retire or drop the procedure only after dependency, privilege, transaction, recovery, and sandbox checks pass.

## Lifecycle Statement Surface

| Operation | Surface | Contract |
| --- | --- | --- |
| Create | `CREATE PROCEDURE` | Creates the durable procedure UUID, input/output descriptors, security mode, dependency graph, source reference, and executable SBLR or UDR binding. |
| Alter | `ALTER PROCEDURE` | Changes admitted metadata such as security mode, package binding, execution policy, result descriptor, or compiled representation. |
| Rename | `RENAME PROCEDURE ... TO ...` | Changes resolver name only; callable identity, grants, dependencies, and executable form remain UUID-bound. |
| Comment | `COMMENT ON PROCEDURE ... IS ...` | Stores authorized descriptive metadata on the procedure catalog row. |
| Show | `SHOW PROCEDURE ...`, `SHOW PROCEDURES` | Returns authorized procedure metadata, signatures, readiness, and package/compiled state. |
| Describe | `DESCRIBE PROCEDURE ...` | Returns parameter descriptors, result descriptors, dependency graph, security mode, body language, and execution binding. |
| Recreate | `RECREATE PROCEDURE ...` | Replaces the procedure through a fresh parse, bind, SBLR/UDR encode, and dependency invalidation route. |
| Drop | `DROP PROCEDURE ... [RESTRICT | CASCADE]` | Retires the procedure only after dependency and overload/signature handling are explicit. |

Stored procedure source text is reference material. Execution must use the admitted encoded representation and engine-owned transaction authority.

## Practical Lifecycle Example

```sql
create procedure app.close_order(p_order_id uuid)
returns (closed_order_id uuid)
as
begin
  update app.orders
     set order_state = 'closed'
   where order_id = p_order_id
  returning order_id into closed_order_id;
  suspend;
end;

alter procedure app.close_order set security definer;
drop procedure app.close_order restrict;
```

## Cursor Parameters

A procedure parameter may use the `cursor` descriptor where policy admits routine-bound row streams.

```sql
create procedure app.copy_order_stream(p_orders cursor)
returns (copied_order_id uuid)
as
begin
  declare v_order_id uuid;
  declare v_order_total decimal(18,2);

  loop
    fetch p_orders into v_order_id, v_order_total;
    if row_not_found() then
      leave;
    end if;

    insert into app.order_copy(order_id, order_total)
    values (v_order_id, v_order_total)
    returning order_id into copied_order_id;

    suspend;
  end loop;
end;
```

Procedure cursor parameters follow the rules in [procedural_sql_cursors.md](procedural_sql_cursors.md):

| Concern | Contract |
| --- | --- |
| Argument value | The caller passes an active cursor handle and descriptor, not copied rows. |
| Ownership | The procedure borrows the cursor unless the signature and policy explicitly admit close, cancel, or transfer behavior. |
| Row shape | Each `FETCH` checks the cursor row descriptor against target descriptors. |
| Transaction context | The procedure uses the cursor's existing snapshot, transaction, holdability, and cleanup rules. |
| Position | Fetching inside the procedure advances the caller-visible cursor handle. |
| Security | The procedure cannot use a passed cursor to bypass row visibility, masks, protected-material policy, or write authorization. |
| Result rows | A procedure may consume a cursor and still emit its own `RETURNS` rows with `SUSPEND`. |

## Boundaries

- User-visible names are resolver input; UUID rows are durable identity.
- The parser cannot create catalog truth by accepting syntax.
- Catalog DDL must be transactionally visible and rollback-safe.
- SBsql parser variants may render SBsql syntax, but catalog authority remains ScratchBird catalog authority.
- Open cursor handles are routine-call resources, not durable procedure metadata.
- Support and diagnostic surfaces may inspect the object only through authorized projections.

## Verification Checklist

| Check | Required Outcome |
| --- | --- |
| Parse | Statement shape is recognized by the SBsql. |
| Bind | Names, UUIDs, descriptors, options, and dependencies resolve exactly. |
| Authorize | The effective user or agent UUID is allowed to mutate the object. |
| Admit | SBLR route and result shape are accepted by the engine verifier. |
| Commit | Catalog mutation becomes visible only through MGA transaction finality. |
| Invalidate | Dependent caches, metadata, plans, and projections are refreshed or refused. |
