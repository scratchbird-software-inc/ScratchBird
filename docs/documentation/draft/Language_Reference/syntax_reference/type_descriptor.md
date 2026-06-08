# Type Descriptor Lifecycle

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `syntax_reference_type_descriptor_lifecycle`


## Purpose

Type descriptors define canonical carrier behavior: storage representation, comparison, hash, ordering, collation, timezone, codec, operation policy, transport profile, and capability flags.

## Complete Lifecycle Model

1. Define the type descriptor with enough descriptor, dependency, security, and policy metadata for the binder and engine verifier to reason about it.
2. Bind the statement to UUID catalog identity and descriptor metadata.
3. Admit the catalog mutation through SBLR and engine verification.
4. Make the mutation visible only when the owning transaction commits.
5. Invalidate dependent plans, parser caches, driver metadata, UDR metadata, support-bundle projections, and metadata rendering views that rely on the changed object.
6. Retire or drop the type descriptor only after dependency, privilege, transaction, recovery, and sandbox checks pass.

## Lifecycle Statement Surface

| Operation | Surface | Contract |
| --- | --- | --- |
| Create | `CREATE TYPE DESCRIPTOR` | Creates a durable descriptor UUID and the canonical type behavior that expressions, storage, casts, indexes, and type alias mappings use. |
| Alter | `ALTER TYPE DESCRIPTOR` | Changes admitted descriptor metadata, capability flags, comparison behavior, cast policy, or type alias mappings without invalid authority shortcuts. |
| Rename | `RENAME TYPE DESCRIPTOR ... TO ...` | Changes only the resolver name; descriptor UUID and dependent object bindings remain stable. |
| Comment | `COMMENT ON TYPE DESCRIPTOR ... IS ...` | Stores authorized descriptive metadata on the descriptor catalog row. |
| Show | `SHOW TYPE DESCRIPTOR ...`, `SHOW TYPE DESCRIPTORS` | Returns authorized descriptor capabilities, storage, comparison, and compatibility projections. |
| Describe | `DESCRIBE TYPE DESCRIPTOR ...` | Returns one descriptor's canonical type shape, capabilities, coercions, casts, collation/charset behavior, and dependencies. |
| Recreate | `RECREATE TYPE DESCRIPTOR ...` | Replaces descriptor metadata only when dependent storage, expression, index, and routine compatibility rules admit it. |
| Drop | `DROP TYPE DESCRIPTOR ... [RESTRICT | CASCADE]` | Retires the descriptor only after all domain, column, routine, and index dependencies are handled. |

Descriptor lifecycle operations are high impact: they define type behavior for data and expressions. Any replacement must preserve or explicitly invalidate dependent compiled, cached, and rendered forms.

## Practical Lifecycle Example

```sql
create type descriptor app.money_amount
  as decimal precision 18 scale 2
  comparison numeric_order
  storage codec default;

alter type descriptor app.money_amount set capability indexable true;
drop type descriptor app.money_amount restrict;
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
