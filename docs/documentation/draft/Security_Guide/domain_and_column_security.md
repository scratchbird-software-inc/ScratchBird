# Domain and Column Security

## Purpose

This page covers the security surfaces below the table level: domain-level
constraints and rights, column-level grants, mask (value-rewriting) rules,
and row-level security (RLS). It explains how these layers compose, how
explicit denial works across them, and how security-epoch invalidation
propagates when they change.

This is a **draft**. No claims herein constitute a production security
certification or a promise of external audit compliance.

Source: `src/engine/internal_api/security/deep_enforcement_api.hpp`,
`deep_enforcement_api.cpp`, `security_model.cpp` (masking in
`EngineEvaluateDeepSecurity`), `security_principal_lifecycle.hpp`
(domain rights in `KnownRights()`).

For syntax reference, see:
- [Language Reference: Policy, Mask, and RLS Lifecycle](../Language_Reference/syntax_reference/policy_mask_and_rls.md)
- [Language Reference: Security and Privilege Statements](../Language_Reference/syntax_reference/security_and_privilege_statements.md)
- [Language Reference: Domain Lifecycle](../Language_Reference/syntax_reference/domain.md)
- [Language Reference: Data Types — Domains, Casts, and Coercion](../Language_Reference/data_types/domains_casts_and_coercion.md)

## Definitions

**Domain** — a named type descriptor that carries constraints, cast rules,
method definitions, and optionally masking and policy configuration. A column
whose type is a domain inherits the domain's security behavior where the
descriptor says it applies.

**Mask** — a durable projection rule that rewrites, redacts, hashes, truncates,
nulls, or otherwise transforms a visible column or domain value before
returning it to the caller. The stored value is unchanged.

**RLS (Row-Level Security)** — a durable filter rule that limits which rows are
visible to the caller or which rows a caller can mutate.

**Deep enforcement** — the `EngineEvaluateDeepSecurity` call that is made
inside the engine executor and storage layer for each operation. Unlike the
parser layer, deep enforcement runs after the statement has been translated to
SBLR and is executing inside the engine.

**Security epoch** — the monotonically increasing counter that tracks mutations
to security state. Policy, mask, and RLS rule changes advance the
`policy_generation` counter and return a `cache_invalidation_epoch`, which
dependent caches must validate before reuse.

## Domain-Level Rights

Source: `security_model.cpp` — `KnownRights()`, `GroupAllows()`.

Domains carry their own right taxonomy, distinct from table rights:

| Right | Purpose |
|-------|---------|
| `DOMAIN_USE` | Use the domain as a column type or cast target |
| `DOMAIN_CAST` | Apply domain cast operations |
| `DOMAIN_METHOD` | Invoke domain methods |
| `DOMAIN_POLICY_ADMIN` | Administer domain-level policy |
| `DOMAIN_UNMASK` | Unmask domain-masked values; bypasses domain mask policy |

`DOMAIN_UNMASK` deserves particular attention. A principal holding
`DOMAIN_UNMASK` for a specific domain can see the raw value of columns whose
mask is applied through that domain. Granting `DOMAIN_UNMASK` to `DEV` group
members triggers an advisory warning from `grant_api.cpp`.

`DBA` group members hold all five domain rights. No other standard group
conveys domain rights by default.

The `UNMASK` right (without the `DOMAIN_` prefix) is a broader unmasking right
checked in `deep_enforcement_api.cpp` during the masking evaluation step. A
principal holding either `UNMASK` or `DOMAIN_UNMASK` for the target passes the
mask bypass check.

## Column-Level Grants

A table-level `GRANT SELECT ON TABLE` admits access to the relation but does
not automatically expose every column. Column-level grants are independent:

```sql
-- table grant: allows targeting the relation
grant select on table app.customer to app_support;

-- column grants: allow specific column projections
grant select on column app.customer.customer_id to app_support;
grant select on column app.customer.display_name to app_support;
grant select on column app.customer.email to app_support;
```

The final column set visible to the caller is the intersection of:
- the table-level grant (confirms the caller can target the relation),
- the column-level grant for each projected column,
- domain mask policy (if the column type is a domain with a mask),
- any standalone mask rule attached to the column,
- any RLS predicate that filters or hides the row containing the column.

Omitting a column grant for a column that a mask policy relies on does not
automatically expose the raw value; the mask still applies even when the column
grant is present.

## Deep Enforcement API

Source: `deep_enforcement_api.hpp`, `deep_enforcement_api.cpp`.

`EngineEvaluateDeepSecurity` is the unified enforcement point for executor and
storage operations. It is not a parser hook; it runs inside the engine after
the parser has translated the statement to SBLR.

Request fields:

| Field | Default | Purpose |
|-------|---------|---------|
| `phase` | `"executor"` | Execution phase: `"executor"`, `"storage"`, `"mutation"`, `"udr"`, `"catalog_discovery"`, `"name_resolution"` |
| `required_right` | `"SELECT"` | Right to check |
| `mutation` | `false` | Whether this is a mutation operation |
| `require_audit_before_success` | `false` | Whether an audit event must be written before the operation can succeed |

Result fields:

| Field | Meaning |
|-------|---------|
| `admitted` | Request reached deep enforcement evaluation |
| `authorized` | Principal holds the required right for the target |
| `visible` | Object is visible to this security context |
| `masked` | Masking policy is active for this value |
| `rls_applied` | A row-level security filter is active |
| `audit_written` | An audit event was written |
| `side_effect_permitted` | Mutation or side-effect is permitted |
| `decision` | `"admitted"`, `"refused"`, or `"hidden_as_missing"` |

The `hidden_as_missing` decision is used when the phase is `"catalog_discovery"`
or `"name_resolution"` and authorization fails. In those phases, the diagnostic
code is `SECURITY.OBJECT.NOT_FOUND_OR_NOT_VISIBLE` rather than
`SECURITY.AUTHORIZATION.DENIED`. This prevents a caller from learning whether
a hidden object exists.

## Masking Evaluation

Source: `deep_enforcement_api.cpp` — masking section.

When `masking_policy` is `"mask"`, the result's `masked = true` and the column
value is transformed by the mask expression before the caller sees it.

When `masking_policy` is `"unmask"`, the engine checks whether the principal
holds `UNMASK` or `DOMAIN_UNMASK` for the target object UUID:

```cpp
result.masked = !SecurityContextHasRight(context, "UNMASK", target_uuid) &&
                !SecurityContextHasRight(context, "DOMAIN_UNMASK", target_uuid);
```

If neither right is held, the value remains masked. If either right is held,
the value is unmasked.

When `masking_policy` is `"none"` (the default), the column value is not
masked.

Masks operate on values after authorization; they do not affect whether the
row is visible.

## RLS Evaluation

Source: `deep_enforcement_api.cpp` — RLS section.

When `rls_policy` is `"filter"`, `result.rls_applied = true` and the row set is
filtered by the RLS predicate. Rows not satisfying the predicate are invisible
to the caller; their existence is not revealed.

When `rls_policy` is `"deny"`, the entire operation is refused with
`SECURITY.RLS.DENIED`. This is the explicit-deny case for RLS: the caller is
not allowed to target this rowset at all under current policy.

When `rls_policy` is `"allow"` (the default), no row-level filtering is applied.

For mutation operations, RLS applies differently by operation type:

| Mutation | RLS Role |
|---------|---------|
| `INSERT` | `WITH CHECK` expression validates the new row image |
| `UPDATE` | `USING` predicate decides whether the old row can be targeted; `WITH CHECK` validates the new image |
| `DELETE` | `USING` predicate decides whether the row can be targeted |

## Composition Rules

Multiple policies, masks, and RLS rules can apply to the same target and
operation.

| Composition Rule | Effect |
|-----------------|--------|
| Explicit deny wins | A denying rule or failed release policy refuses access even when another rule would allow it |
| Restrictive composition | The row or value must satisfy every applicable restrictive rule |
| Permissive composition | The row or value may be admitted by one permissive rule only if no stronger rule denies it |
| Object-specific rules | Table, column, domain, and protected-material rules all apply where their scopes intersect |

Source: `policy_mask_and_rls.md` (Language Reference).

When composition is ambiguous, the operation fails closed. The engine does not
infer allow behavior from missing policy rows.

## Audit Before Success

Source: `deep_enforcement_api.cpp`.

When `require_audit_before_success` is true, or when a mutation (`mutation =
true`) is detected, the engine calls `AppendSecurityEvidenceEvent` to write an
audit event before the operation is permitted. If the audit write fails, the
operation is refused.

This is the `audit_evidence_required` policy in action: certain operations must
produce audit evidence before they are allowed to succeed. If the audit pathway
is unavailable, the operation fails closed.

## Security Epoch Invalidation

Policy, mask, and RLS mutations advance `policy_generation` (and return a
`cache_invalidation_epoch`). All of the following are invalidated when the
policy epoch advances:

- prepared statements
- query plans and optimizer evidence
- parser metadata caches
- driver metadata
- catalog projections
- support-bundle manifests
- diagnostic renderers
- security snapshots
- view and materialized-view readiness where applicable

This invalidation is not optional. Stale policy is refused, not silently
applied.

Source: `security_principal_lifecycle.hpp` — `EngineSecurityValidatePolicyCache`.

## Column Security Example

```sql
-- create a table with an email column
create table app.customer (
  customer_id   uuid not null,
  display_name  text not null,
  email         text not null
);

-- grant table and column access to the support role
grant select on table app.customer to role app_support;
grant select on column app.customer.customer_id to role app_support;
grant select on column app.customer.display_name to role app_support;
grant select on column app.customer.email to role app_support;

-- mask the email column: return raw value for privileged role,
-- redacted string for everyone else
create mask app.customer_email_mask
on column app.customer.email
using case
  when has_role('app_support_private') then email
  else 'redacted'
end
to role app_support
active;
```

The `app_support` role can project the `email` column but will see `'redacted'`
unless they are also a member of `app_support_private`. The mask expression
does not alter the stored value.

## RLS Example

```sql
-- tenant-isolated row visibility
create rls app.orders_tenant_rls
on table app.orders
for select, update, delete
to role app_user
using (tenant_uuid = current_tenant_uuid())
with check (tenant_uuid = current_tenant_uuid())
as restrictive
active;
```

The `USING` predicate limits which rows are visible for read and mutation
targeting. The `WITH CHECK` predicate limits which row images are allowed for
insert and update. Rows outside the caller's tenant are invisible; their
existence is not revealed.

## Interaction with Domains

A column defined with a domain type inherits the domain's masking and policy
configuration where the domain descriptor says it applies.

```sql
create domain app.tenant_id as uuid
  -- domain carries its own visibility and casting policy
  ;

create table app.orders (
  order_id  app.tenant_id not null,
  ...
);
```

If the `app.tenant_id` domain carries a mask policy, columns of that type in
`app.orders` will be masked accordingly. The domain mask applies in addition to
any column-specific mask. If both apply, the composed result follows the
restrictive composition rule: both masks must admit the value.

## Interaction with Materialized Views

Materialized views must record whether their stored rows already include
policy-filtered data, whether refresh runs as invoker or definer, and whether a
read from the materialized view rechecks caller policy. If that state is
ambiguous, refresh or read access fails closed.

A view grant is not a grant on the underlying base table. If a caller has
`SELECT` on a view but not on the base table, and the view does not run as the
definer, access to the underlying rows is still subject to the base table's
grants, column grants, masks, and RLS.

## Invariants

- Visibility is the intersection of MGA visibility and materialized security
  policy. A mask does not expose a row that RLS hides, and RLS does not expose
  a value that a mask rewrites.
- The system is fail-closed. Missing, stale, ambiguous, or corrupted policy
  state refuses the operation.
- Explicit denial wins over allow at every composition level.
- The `hidden_as_missing` decision in catalog_discovery and name_resolution
  phases prevents callers from learning whether a hidden object exists.
- Mask and RLS mutations advance the policy epoch and invalidate all dependent
  caches.

## Related Pages

- [grants_and_privileges.md](grants_and_privileges.md) — table-level and
  object-level grants
- [standard_roles_and_groups.md](standard_roles_and_groups.md) — which groups
  hold domain rights
- [Language Reference: Policy, Mask, and RLS Lifecycle](../Language_Reference/syntax_reference/policy_mask_and_rls.md)
- [Language Reference: Security and Privilege Statements](../Language_Reference/syntax_reference/security_and_privilege_statements.md)
- [Language Reference: Security and Sandboxing](../Language_Reference/core_paradigms/security_and_sandboxing.md)
