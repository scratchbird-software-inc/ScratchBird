# Sequence Lifecycle

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `syntax_reference_sequence_lifecycle`


## Purpose

Sequences produce generated values through catalog-controlled state. Cache, restart, cycle, ownership, transaction interaction, and privilege behavior must be explicit.

## Complete Lifecycle Model

1. Define the sequence with enough descriptor, dependency, security, and policy metadata for the binder and engine verifier to reason about it.
2. Bind the statement to UUID catalog identity and descriptor metadata.
3. Admit the catalog mutation through SBLR and engine verification.
4. Make the mutation visible only when the owning transaction commits.
5. Invalidate dependent plans, parser caches, driver metadata, UDR metadata, support-bundle projections, and donor compatibility views that rely on the changed object.
6. Retire or drop the sequence only after dependency, privilege, transaction, recovery, and sandbox checks pass.

## Lifecycle Statement Surface

| Operation | Surface | Contract |
| --- | --- | --- |
| Create | `CREATE SEQUENCE` | Creates the durable sequence UUID, range, increment, cache policy, cycle behavior, and ownership metadata. |
| Alter | `ALTER SEQUENCE` | Changes admitted range, restart, increment, cache, cycle, ownership, or policy metadata. |
| Rename | `RENAME SEQUENCE ... TO ...` | Changes the resolver name only; current value authority and dependency edges remain bound to the sequence UUID. |
| Comment | `COMMENT ON SEQUENCE ... IS ...` | Stores authorized descriptive metadata on the sequence catalog row. |
| Show | `SHOW SEQUENCE ...`, `SHOW SEQUENCES` | Returns authorized sequence metadata and current-state projection according to policy. |
| Describe | `DESCRIBE SEQUENCE ...` | Returns one sequence's descriptor, range, cache, cycle, ownership, dependency, and visibility metadata. |
| Recreate | `RECREATE SEQUENCE ...` | Replaces sequence metadata only when restart and dependency behavior are explicitly admitted. |
| Drop | `DROP SEQUENCE ... [RESTRICT | CASCADE]` | Retires the sequence only after default, generated-column, routine, and donor compatibility dependencies are handled. |

Sequence inspection does not grant value-generation authority. `NEXT VALUE`, donor generator aliases, and restart behavior remain separate execution surfaces with their own rights and transaction policy.

## Practical Lifecycle Example

```sql
create sequence app.order_number
  start with 1
  increment by 1
  no cycle;

alter sequence app.order_number restart with 100000;
drop sequence app.order_number restrict;
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
