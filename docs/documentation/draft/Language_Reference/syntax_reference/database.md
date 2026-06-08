# Database Lifecycle

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `syntax_reference_database_lifecycle`


## Purpose

Database lifecycle statements control database-level catalog and operational metadata. Physical create/open/reopen/recovery behavior remains engine-managed and can fail closed when state is uncertain.

## Complete Lifecycle Model

1. Define the database with enough descriptor, dependency, security, and policy metadata for the binder and engine verifier to reason about it.
2. Bind the statement to UUID catalog identity and descriptor metadata.
3. Admit the catalog mutation through SBLR and engine verification.
4. Make the mutation visible only when the owning transaction commits.
5. Invalidate dependent plans, parser caches, driver metadata, UDR metadata, support-bundle projections, and donor compatibility views that rely on the changed object.
6. Retire or drop the database only after dependency, privilege, transaction, recovery, and sandbox checks pass.

## Lifecycle Statement Surface

| Operation | Surface | Contract |
| --- | --- | --- |
| Create | `CREATE DATABASE` | Creates database-level catalog metadata and requests engine-managed physical initialization according to policy. |
| Alter | `ALTER DATABASE` | Changes admitted database metadata such as default schema, character set, policy, filespace binding, or operational mode. |
| Rename | `RENAME DATABASE ... TO ...` | Changes the database resolver name only; file identity, catalog UUIDs, security state, and recovery metadata remain engine-owned. |
| Comment | `COMMENT ON DATABASE ... IS ...` | Stores authorized descriptive metadata on the database catalog row. |
| Show | `SHOW DATABASE ...`, `SHOW DATABASES` | Returns authorized database metadata, readiness, recovery, policy, filespace, and compatibility projections. |
| Describe | `DESCRIBE DATABASE ...` | Returns one database's authorized catalog, storage, security, policy, and lifecycle summary. |
| Recreate | `RECREATE DATABASE ...` | Requests a full replacement lifecycle and must fail closed unless data-retention, recovery, lock, and dependency policy admit replacement. |
| Drop | `DROP DATABASE ... [RESTRICT | CASCADE]` | Retires or removes the database only through engine-managed close, dependency, recovery, and storage checks. |

Database lifecycle statements are not physical file operations performed by the parser. The engine owns create/open/close/reopen, recovery-required state, and fail-closed diagnostics.

## Practical Lifecycle Example

```sql
create database tenant_a
  with default character set utf8
  policy production_default;

alter database tenant_a set default schema app;
comment on database tenant_a is 'Tenant A production database';
show database tenant_a;
describe database tenant_a;
rename database tenant_a to tenant_a_live;
drop database tenant_a_live restrict;
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
