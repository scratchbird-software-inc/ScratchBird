# Domain Lifecycle

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `syntax_reference_domain_lifecycle`


## Purpose

Domains wrap descriptors with null policy, defaults, constraints, masks, donor-facing type metadata, cast policy, and operation policy. Domain use preserves domain semantics unless a cast policy explicitly erases them.

## Complete Lifecycle Model

1. Define the domain with enough descriptor, dependency, security, and policy metadata for the binder and engine verifier to reason about it.
2. Bind the statement to UUID catalog identity and descriptor metadata.
3. Admit the catalog mutation through SBLR and engine verification.
4. Make the mutation visible only when the owning transaction commits.
5. Invalidate dependent plans, parser caches, driver metadata, UDR metadata, support-bundle projections, and donor compatibility views that rely on the changed object.
6. Retire or drop the domain only after dependency, privilege, transaction, recovery, and sandbox checks pass.

## Lifecycle Statement Surface

| Operation | Surface | Contract |
| --- | --- | --- |
| Create | `CREATE DOMAIN` | Creates the durable domain UUID, underlying descriptor reference, defaults, constraints, nullability, cast policy, and donor-facing metadata. |
| Alter | `ALTER DOMAIN` | Changes admitted domain defaults, constraints, null policy, descriptor binding, masks, or compatibility metadata. |
| Rename | `RENAME DOMAIN ... TO ...` | Changes the resolver name only; dependent columns and routines remain bound to the domain UUID. |
| Comment | `COMMENT ON DOMAIN ... IS ...` | Stores authorized descriptive metadata on the domain catalog row. |
| Show | `SHOW DOMAIN ...`, `SHOW DOMAINS` | Returns authorized domain metadata and dependency projections. |
| Describe | `DESCRIBE DOMAIN ...` | Returns descriptor, constraint, nullability, default, cast, donor mapping, and dependency details for one domain. |
| Recreate | `RECREATE DOMAIN ...` | Replaces the domain definition only if dependent descriptors and data compatibility rules admit the replacement. |
| Drop | `DROP DOMAIN ... [RESTRICT | CASCADE]` | Retires the domain only after dependency and descriptor compatibility checks pass. |

Domain lifecycle operations preserve descriptor authority. A parser-visible donor type name never overrides the canonical ScratchBird descriptor and domain UUID binding.

## Practical Lifecycle Example

```sql
create domain app.positive_amount as decimal(18, 2)
  default 0
  check (value >= 0);

alter domain app.positive_amount
  set default 0.00;

comment on domain app.positive_amount is 'Non-negative currency amount';
show domain app.positive_amount;
describe domain app.positive_amount;
rename domain app.positive_amount to nonnegative_amount;
drop domain app.nonnegative_amount restrict;
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
