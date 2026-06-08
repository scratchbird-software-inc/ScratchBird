# Filespace Lifecycle

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `syntax_reference_filespace_lifecycle`


## Purpose

Filespaces describe storage placement, allocation policy, growth limits, sync policy, and recovery fencing. SBsql requests the change; storage code owns allocation and durability behavior.

## Complete Lifecycle Model

1. Define the filespace with enough descriptor, dependency, security, and policy metadata for the binder and engine verifier to reason about it.
2. Bind the statement to UUID catalog identity and descriptor metadata.
3. Admit the catalog mutation through SBLR and engine verification.
4. Make the mutation visible only when the owning transaction commits.
5. Invalidate dependent plans, parser caches, driver metadata, UDR metadata, support-bundle projections, and donor compatibility views that rely on the changed object.
6. Retire or drop the filespace only after dependency, privilege, transaction, recovery, and sandbox checks pass.

## Lifecycle Statement Surface

| Operation | Surface | Contract |
| --- | --- | --- |
| Create | `CREATE FILESPACE` | Creates the durable filespace metadata, storage policy, growth limits, and integrity requirements. |
| Alter | `ALTER FILESPACE` | Changes admitted filespace policy such as growth, placement, read-only state, threshold, or maintenance settings. |
| Rename | `RENAME FILESPACE ... TO ...` | Changes the resolver name only; physical placement and storage identity remain engine-owned. |
| Comment | `COMMENT ON FILESPACE ... IS ...` | Stores authorized descriptive metadata for operators and support bundles. |
| Show | `SHOW FILESPACE ...`, `SHOW FILESPACES` | Returns authorized storage health, readiness, limits, and placement metadata. |
| Describe | `DESCRIBE FILESPACE ...` | Returns one filespace's authorized capacity, policy, integrity, and dependency summary. |
| Recreate | `RECREATE FILESPACE ...` | Replaces filespace metadata only when no unsafe data movement, dependency, or recovery conflict exists. |
| Drop | `DROP FILESPACE ... [RESTRICT | CASCADE]` | Retires the filespace only after storage dependencies and recovery fences are resolved. |

Filespace statements describe engine-owned storage policy. They do not allow arbitrary server-local file access through SQL text or parser behavior.

## Practical Lifecycle Example

```sql
create filespace fast_data
  location 'policy://storage/fast_data'
  max size 500 gb;

alter filespace fast_data set max size 750 gb;
show filespace fast_data;
drop filespace fast_data restrict;
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
