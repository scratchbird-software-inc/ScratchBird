# Policy, Mask, And RLS Lifecycle

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `syntax_reference_policy_mask_rls_lifecycle`


## Purpose

Policies, masks, and row-level security rules are durable authorization objects. They affect visibility, mutation, rendering, diagnostics, and support-bundle redaction after they are materialized into the active security context.

## Complete Lifecycle Model

1. Define the policy, mask, and row-level security rule with enough descriptor, dependency, security, and policy metadata for the binder and engine verifier to reason about it.
2. Bind the statement to UUID catalog identity and descriptor metadata.
3. Admit the catalog mutation through SBLR and engine verification.
4. Make the mutation visible only when the owning transaction commits.
5. Invalidate dependent plans, parser caches, driver metadata, UDR metadata, support-bundle projections, and metadata rendering views that rely on the changed object.
6. Retire or drop the policy, mask, and row-level security rule only after dependency, privilege, transaction, recovery, and sandbox checks pass.

## Lifecycle Statement Surface

| Operation | Surface | Contract |
| --- | --- | --- |
| Create | `CREATE POLICY`, `CREATE MASK`, `CREATE RLS` | Creates durable authorization objects with target bindings, predicates or expressions, protected-material handling, dependency edges, and enforcement policy. |
| Alter | `ALTER POLICY`, `ALTER MASK`, `ALTER RLS` | Changes admitted enablement, target binding, predicate/expression, principal scope, priority, or enforcement metadata. |
| Rename | `RENAME POLICY`, `RENAME MASK`, `RENAME RLS` | Changes the resolver name only; security object UUID and enforcement bindings remain stable. |
| Comment | `COMMENT ON POLICY`, `COMMENT ON MASK`, `COMMENT ON RLS` | Stores authorized descriptive metadata without weakening enforcement. |
| Show | `SHOW POLICY`, `SHOW MASK`, `SHOW RLS` | Returns authorized security metadata and enforcement readiness. It must redact protected expressions or principal detail when policy requires it. |
| Describe | `DESCRIBE POLICY`, `DESCRIBE MASK`, `DESCRIBE RLS` | Returns one object's target binding, predicate/expression descriptor, principal scope, enforcement state, and dependency metadata according to caller authority. |
| Recreate | `RECREATE POLICY`, `RECREATE MASK`, `RECREATE RLS` | Replaces security behavior only through fresh authorization checks and dependency invalidation. |
| Drop | `DROP POLICY`, `DROP MASK`, `DROP RLS` | Retires the security object only when fail-open behavior is impossible; if uncertainty exists, protected access must fail closed. |

Security lifecycle inspection is itself security-sensitive. `SHOW` and `DESCRIBE` may return redacted metadata when the caller can observe the existence of an object but not its protected internals.

## Practical Lifecycle Example

```sql
create policy app.orders_tenant_policy
on table app.orders
using (tenant_uuid = current_tenant_uuid());

create mask app.customer_email_mask
on table app.customer column email
using case when has_role('support') then email else null end;

alter policy app.orders_tenant_policy enable;
comment on policy app.orders_tenant_policy is 'Tenant isolation policy for orders';
show policy app.orders_tenant_policy;
describe policy app.orders_tenant_policy;
rename policy app.orders_tenant_policy to orders_tenant_isolation;
drop policy app.orders_tenant_isolation restrict;
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
