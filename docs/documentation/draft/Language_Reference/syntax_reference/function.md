# Function Lifecycle

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `syntax_reference_function_lifecycle`


## Purpose

Functions are executable catalog objects. The original SBsql text is retained as reference text, while the executable form must be bound into UUID-aware, descriptor-aware SBLR or trusted UDR metadata for execution, JIT/AOT eligibility, dependency analysis, and audit.

The procedural body language is documented in [procedural_sql.md](procedural_sql.md), with detailed coverage for [blocks and declarations](procedural_sql_blocks.md), [control flow](procedural_sql_control_flow.md), [cursors](procedural_sql_cursors.md), and [exceptions and diagnostics](procedural_sql_exceptions.md).

## Complete Lifecycle Model

1. Define the function with enough descriptor, dependency, security, and policy metadata for the binder and engine verifier to reason about it.
2. Bind the statement to UUID catalog identity and descriptor metadata.
3. Admit the catalog mutation through SBLR and engine verification.
4. Make the mutation visible only when the owning transaction commits.
5. Invalidate dependent plans, parser caches, driver metadata, UDR metadata, support-bundle projections, and metadata rendering views that rely on the changed object.
6. Retire or drop the function only after dependency, privilege, transaction, recovery, and sandbox checks pass.

## Lifecycle Statement Surface

| Operation | Surface | Contract |
| --- | --- | --- |
| Create | `CREATE FUNCTION` | Creates the durable function UUID, signature descriptors, security mode, dependency graph, original source reference, and executable SBLR or UDR binding. |
| Alter | `ALTER FUNCTION` | Changes admitted metadata such as determinism, security mode, cost, volatility, native/JIT/AOT eligibility, package binding, or policy. |
| Rename | `RENAME FUNCTION ... TO ...` | Changes resolver name only; overload identity, signature descriptors, grants, and executable form remain UUID-bound. |
| Comment | `COMMENT ON FUNCTION ... IS ...` | Stores authorized descriptive metadata on the function catalog row. |
| Show | `SHOW FUNCTION ...`, `SHOW FUNCTIONS` | Returns authorized function metadata, overloads, package binding, readiness, and compilation state. |
| Describe | `DESCRIBE FUNCTION ...` | Returns signature descriptors, return type, body language, dependency graph, security mode, and execution binding for one overload or overload set. |
| Recreate | `RECREATE FUNCTION ...` | Replaces the function definition through a fresh parse, bind, SBLR/UDR encode, and dependency invalidation route. |
| Drop | `DROP FUNCTION ... [RESTRICT | CASCADE]` | Retires the function only after overload resolution and dependency handling are explicit. |

Function lifecycle is not text storage alone. The body must be encoded into a functional representation suitable for validation, execution, audit, and JIT/AOT eligibility where admitted.

## Practical Lifecycle Example

```sql
create function app.tax_amount(amount decimal(18,2), rate decimal(9,6))
returns decimal(18,2)
as
begin
  return amount * rate;
end;

alter function app.tax_amount set deterministic true;
comment on function app.tax_amount is 'Computes tax from amount and rate';
show function app.tax_amount;
describe function app.tax_amount;
rename function app.tax_amount to tax_amount_v1;
drop function app.tax_amount_v1 restrict;
```

## Boundaries

- User-visible names are resolver input; UUID rows are durable identity.
- The parser cannot create catalog truth by accepting syntax.
- Catalog DDL must be transactionally visible and rollback-safe.
- SBsql parser variants may render SBsql syntax, but catalog authority remains ScratchBird catalog authority.
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
