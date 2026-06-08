# Schema Lifecycle

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `syntax_reference_schema_lifecycle`


## Purpose

Schemas are resolver scopes in the schema tree. Creating a schema creates a namespace branch; altering or renaming one changes resolver metadata; dropping one requires dependency and privilege checks.

The schema-tree model, session schema variables, recursive branch layout, name-resolution order, search-path behavior, and donor-parser sandbox rules are documented in [schema_tree_and_name_resolution.md](schema_tree_and_name_resolution.md).

## Complete Lifecycle Model

1. Define the schema with enough descriptor, dependency, security, and policy metadata for the binder and engine verifier to reason about it.
2. Bind the statement to UUID catalog identity and descriptor metadata.
3. Admit the catalog mutation through SBLR and engine verification.
4. Make the mutation visible only when the owning transaction commits.
5. Invalidate dependent plans, parser caches, driver metadata, UDR metadata, support-bundle projections, and donor compatibility views that rely on the changed object.
6. Retire or drop the schema only after dependency, privilege, transaction, recovery, and sandbox checks pass.

## Lifecycle Statement Surface

| Operation | Surface | Contract |
| --- | --- | --- |
| Create | `CREATE SCHEMA` | Creates a durable schema UUID and resolver branch in the schema tree. |
| Alter | `ALTER SCHEMA` | Mutates admitted schema metadata, default policy, owner, or visibility settings without changing object identity. |
| Rename | `RENAME SCHEMA ... TO ...` | Changes the schema branch name. Child object UUIDs and dependencies remain unchanged, but resolver and cache projections must be invalidated. |
| Comment | `COMMENT ON SCHEMA ... IS ...` | Stores authorized descriptive metadata on the schema catalog row. |
| Show | `SHOW SCHEMA ...`, `SHOW SCHEMAS` | Returns only authorized schema branches and properties; sandboxed donor sessions see their emulated root as their schema root. |
| Describe | `DESCRIBE SCHEMA ...` | Returns authorized child object classes, default policies, owner, and branch metadata for one schema. |
| Recreate | `RECREATE SCHEMA ...` | Replaces the schema branch only when dependency, sandbox, and recovery rules admit the operation. |
| Drop | `DROP SCHEMA ... [RESTRICT | CASCADE]` | Removes or retires the schema branch only after child-object dependency handling is explicit and authorized. |

Schema lifecycle operations operate on resolver scope. They must not expose private parent branches to donor-parser sessions that are sandboxed below their emulated database root.

## Practical Lifecycle Example

```sql
create schema app;
comment on schema app is 'Application-owned objects';
show schema app;
describe schema app;
rename schema app to app_archive;
recreate schema app_stage;
drop schema app_archive restrict;
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
