# Policy, Mask, And RLS Lifecycle

This page is part of the SBsql Language Reference Manual. It explains the user-facing policy, mask, and row-level security contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `syntax_reference_policy_mask_rls_lifecycle`

Related pages: [Security And Privilege Statements](security_and_privilege_statements.md), [Security And Sandboxing](../core_paradigms/security_and_sandboxing.md), [Table Lifecycle](table.md), [View Lifecycle](view.md), [Domain Lifecycle](domain.md), [Trigger Lifecycle](trigger.md), [Transaction Control](transaction_control.md), [Protected Material Policy Binding](../catalog_reference/sys_catalog_protected_material_policy_binding.md), and [Refusal Vectors](refusal_vectors.md).

## Purpose

Policies, masks, and row-level security rules are durable authorization objects. They refine ordinary grants by controlling whether rows are visible or mutable, whether column values are rendered directly or transformed, whether diagnostics are redacted, and whether protected material can be released.

These objects do not grant authority by existing. They are enforced after the effective user or agent UUID, active roles, groups, sandbox root, object grants, column grants, and policy snapshot have been materialized. They are fail-closed: if policy state is missing, ambiguous, stale, corrupted, unauthorized, or recovery-fenced, protected access is refused or redacted rather than silently allowed.

## Object Families

| Object | Purpose | Typical target |
| --- | --- | --- |
| Policy | General durable security rule that can be attached to a table, view, schema, domain, protected material, routine, bridge, management surface, or support surface. | Object UUID plus operation family. |
| Mask | Projection rule that rewrites, redacts, hashes, truncates, nulls, or otherwise transforms a visible value before returning it to the caller. | Column, domain, domain element, view column, protected value, diagnostic field, or support-bundle field. |
| RLS | Row-level rule that filters rows and checks row mutation admission for a table or view-like rowset. | Table or view row descriptor. |

RLS means row-level security. It is separate from row locking, transaction visibility, and MGA version visibility. MGA decides which row versions exist for a transaction. RLS decides which of those otherwise visible rows the effective security context may see or mutate.

## Core Concepts

| Concept | Meaning |
| --- | --- |
| Target object | The durable UUID of the table, view, domain, column, protected value, or surface governed by the rule. |
| Principal scope | The users, roles, groups, agents, or public scope to which the rule applies. |
| Operation scope | The operations governed by the rule, such as `SELECT`, `INSERT`, `UPDATE`, `DELETE`, `DESCRIBE`, support-bundle generation, or diagnostic rendering. |
| Predicate | Boolean expression evaluated against a row, security context, session context, or metadata descriptor. |
| Mask expression | Expression that produces the value returned to the caller instead of the stored value. |
| Check expression | Boolean expression that a proposed row image must satisfy before mutation. |
| Enforcement mode | Whether the rule is active, inactive, validating, audit-only, dry-run, or policy-defined. |
| Composition | How multiple rules combine: explicit denial wins, restrictive rules narrow access, and permissive rules can admit only where no stronger rule refuses. |
| Security epoch | Monotonic security generation used to invalidate plans, statements, metadata, and support projections after a security change. |

## Statement Surface

| Operation | Surface | Contract |
| --- | --- | --- |
| Create policy | `CREATE POLICY` | Creates a durable policy identity with target binding, operation scope, principal scope, enforcement metadata, and optional predicate or option payload. |
| Alter policy | `ALTER POLICY` | Changes admitted policy metadata such as active state, target, scope, expression, priority, validation state, or option payload. |
| Create mask | `CREATE MASK` | Creates a durable masking rule with target column/domain/value binding, mask expression, disclosure level, and protected-material behavior. |
| Alter mask | `ALTER MASK` | Changes admitted mask expression, disclosure level, active state, target binding, validation state, or redaction metadata. |
| Create RLS | `CREATE RLS` | Creates a durable row-level rule with row visibility predicate and optional mutation check expression. |
| Alter RLS | `ALTER RLS` | Changes row predicate, check expression, principal scope, active state, validation mode, or priority. |
| Rename | `RENAME POLICY`, `RENAME MASK`, `RENAME RLS` | Changes resolver names only; durable UUID identity and dependency edges remain stable. |
| Comment | `COMMENT ON POLICY`, `COMMENT ON MASK`, `COMMENT ON RLS` | Stores authorized descriptive metadata without weakening enforcement. |
| Show | `SHOW POLICIES`, `SHOW MASKS`, `SHOW RLS` | Lists authorized metadata and readiness state. Protected internals may be redacted. |
| Describe | `DESCRIBE POLICY`, `DESCRIBE MASK`, `DESCRIBE RLS` | Shows one object's binding, expression descriptors, dependency metadata, and enforcement state according to disclosure policy. |
| Validate | `VALIDATE POLICY`, `VALIDATE MASK`, `VALIDATE RLS` | Checks target existence, expression binding, dependencies, privileges, and fail-closed behavior without silently enabling unsafe rules. |
| Drop | `DROP POLICY`, `DROP MASK`, `DROP RLS` | Retires the security object only when dependency, privilege, transaction, recovery, and fail-closed checks pass. |

## Syntax

SBsql is context sensitive. The words shown here are command words in this statement family and do not need to be globally reserved elsewhere.

```ebnf
policy_mask_rls_statement ::=
      create_policy_statement
    | alter_policy_statement
    | drop_policy_statement
    | rename_policy_statement
    | comment_policy_statement
    | show_policy_statement
    | describe_policy_statement
    | validate_policy_statement
    | create_mask_statement
    | alter_mask_statement
    | drop_mask_statement
    | rename_mask_statement
    | comment_mask_statement
    | show_mask_statement
    | describe_mask_statement
    | validate_mask_statement
    | create_rls_statement
    | alter_rls_statement
    | drop_rls_statement
    | rename_rls_statement
    | comment_rls_statement
    | show_rls_statement
    | describe_rls_statement
    | validate_rls_statement ;
```

```ebnf
create_policy_statement ::=
    "CREATE" "POLICY" policy_name
    "ON" policy_target
    policy_clause* ;

alter_policy_statement ::=
    "ALTER" "POLICY" policy_ref alter_policy_action+ ;

drop_policy_statement ::=
    "DROP" "POLICY" policy_ref drop_behavior? ;
```

```ebnf
create_mask_statement ::=
    "CREATE" "MASK" mask_name
    "ON" mask_target
    "USING" mask_expression
    mask_clause* ;

alter_mask_statement ::=
    "ALTER" "MASK" mask_ref alter_mask_action+ ;

drop_mask_statement ::=
    "DROP" "MASK" mask_ref drop_behavior? ;
```

```ebnf
create_rls_statement ::=
    "CREATE" "RLS" rls_name
    "ON" rowset_target
    rls_clause* ;

alter_rls_statement ::=
    "ALTER" "RLS" rls_ref alter_rls_action+ ;

drop_rls_statement ::=
    "DROP" "RLS" rls_ref drop_behavior? ;
```

```ebnf
policy_target ::=
      "DATABASE" qualified_name
    | "SCHEMA" qualified_name
    | "TABLE" qualified_name
    | "VIEW" qualified_name
    | "MATERIALIZED" "VIEW" qualified_name
    | "COLUMN" qualified_name "." identifier
    | "DOMAIN" qualified_name
    | "FUNCTION" qualified_name
    | "PROCEDURE" qualified_name
    | "BRIDGE" qualified_name
    | "SYSTEM" qualified_name ;

mask_target ::=
      "COLUMN" qualified_name "." identifier
    | "DOMAIN" qualified_name
    | "DOMAIN" "ELEMENT" qualified_name "." identifier
    | "PROTECTED" "MATERIAL" qualified_name
    | "DIAGNOSTIC" qualified_name
    | "SUPPORT" "FIELD" qualified_name ;

rowset_target ::=
      "TABLE" qualified_name
    | "VIEW" qualified_name ;
```

```ebnf
policy_clause ::=
      "FOR" policy_operation_list
    | "TO" principal_scope
    | "USING" predicate
    | "WITH" "CHECK" predicate
    | "AS" composition_mode
    | enforcement_clause
    | policy_option ;

rls_clause ::=
      "FOR" row_operation_list
    | "TO" principal_scope
    | "USING" predicate
    | "WITH" "CHECK" predicate
    | "AS" composition_mode
    | enforcement_clause
    | policy_option ;

mask_clause ::=
      "TO" principal_scope
    | "FOR" policy_operation_list
    | enforcement_clause
    | mask_option ;

enforcement_clause ::=
      "ACTIVE"
    | "INACTIVE"
    | "VALIDATE"
    | "AUDIT" "ONLY"
    | "DRY" "RUN" ;

composition_mode ::=
      "RESTRICTIVE"
    | "PERMISSIVE" ;
```

The exact option set is policy defined. Unsupported options must be refused with a canonical message vector; they must not be accepted as inert text.

## CREATE POLICY

`CREATE POLICY` creates a durable policy identity. The minimal public route shape is:

```sql
create policy app_policy on table customer;
```

A complete policy may include operation scope, principal scope, predicates, check expressions, and enforcement metadata:

```sql
create policy app.orders_support_visibility
on table app.orders
for select
to role app_support
using (
  tenant_uuid = current_tenant_uuid()
  and order_state <> 'sealed'
)
as restrictive
active;
```

Creation must bind the target object UUID and validate that the expression descriptors can be evaluated under the target row descriptor and security context. The parser cannot create security truth by accepting the text. The engine creates the durable policy row only through an admitted catalog/security route.

## ALTER POLICY

`ALTER POLICY` changes an existing policy. The public route includes inactive-state alteration:

```sql
alter policy app_policy set inactive;
```

Other admitted alterations use the same authority model:

```sql
alter policy app.orders_support_visibility set active;
alter policy app.orders_support_visibility set audit only;
alter policy app.orders_support_visibility to role app_auditor;
alter policy app.orders_support_visibility using (
  tenant_uuid = current_tenant_uuid()
);
```

Altering a policy advances the security epoch and invalidates prepared statements, plan caches, metadata renderings, support-bundle projections, parser metadata caches, and driver metadata that depended on the old policy state.

## CREATE MASK

`CREATE MASK` defines the visible value returned for a protected target. It does not change the stored value.

```sql
create mask app.customer_email_support_mask
on column app.customer.email
using case
  when has_role('app_support') then email
  else null
end
to role app_support
active;
```

Common mask patterns:

| Pattern | Contract |
| --- | --- |
| Nulling | Returns `NULL` when the value exists but the caller cannot see it. |
| Constant redaction | Returns a constant such as `'redacted'` according to descriptor type. |
| Partial rendering | Returns a substring, prefix, suffix, or formatted value that policy admits. |
| Hashing | Returns a deterministic or salted digest where policy and descriptor admit it. |
| Protected release | Returns a raw value only when protected-material release policy admits it. |
| Diagnostic redaction | Rewrites diagnostic or support fields before display or bundle generation. |

The mask expression must produce a value compatible with the target descriptor or with an explicitly declared output descriptor. Silent type drift is refused.

## CREATE RLS

`CREATE RLS` defines row-level visibility and optional mutation checks for a rowset.

```sql
create rls app.orders_tenant_rls
on table app.orders
for select, update, delete
to role app_user
using (tenant_uuid = current_tenant_uuid())
with check (tenant_uuid = current_tenant_uuid())
as restrictive
active;
```

`USING` controls which existing rows are visible for the operation. `WITH CHECK` controls whether a proposed inserted or updated row image is allowed. A rule that has `USING` but no `WITH CHECK` does not automatically admit writes unless policy says the visibility predicate also acts as a check predicate.

## Enforcement Order

For an ordinary row read, ScratchBird applies security in this conceptual order:

1. Authenticate the session or agent and materialize the effective UUID.
2. Establish sandbox root, attached database/workarea, current schema, active role set, and group membership.
3. Resolve the target object under name visibility and metadata rules.
4. Check object-level grants.
5. Check column, element, domain, and protected-material grants.
6. Apply explicit deny rules.
7. Apply RLS row visibility rules to otherwise MGA-visible rows.
8. Apply mask and protected-material release rules to projected values.
9. Redact diagnostics, support-bundle fields, and metadata according to policy.
10. Return the admitted rowset or a canonical refusal message vector.

For mutation, the engine also checks the proposed row image:

| Operation | RLS and mask interaction |
| --- | --- |
| `INSERT` | `WITH CHECK` rules validate the new row image. Masks do not change stored values. |
| `UPDATE` | `USING` decides whether the old row can be targeted. `WITH CHECK` validates the new row image. |
| `DELETE` | `USING` decides whether the row can be targeted. There is no new row image. |
| `MERGE` and UPSERT | The matched and not-matched paths apply the relevant `UPDATE`, `DELETE`, or `INSERT` rules. |
| `SELECT FOR UPDATE` or locking reads | Row visibility and mutation eligibility are checked before the row can be locked for mutation. |

Triggers, generated columns, defaults, constraints, domains, and protected-value release rules still run through their own authority and descriptor checks. A trigger body does not bypass RLS or masks unless an explicit definer/security policy admits a different context.

## Composition Rules

Multiple policies can apply to the same target and operation.

| Rule | Meaning |
| --- | --- |
| Explicit deny wins | A denying rule or failed protected-material release refuses access even when another rule would allow it. |
| Restrictive composition | The row or value must satisfy every applicable restrictive rule. |
| Permissive composition | The row or value may be admitted by one applicable permissive rule only if no stronger rule denies it. |
| Principal-specific rules | User, role, group, and agent scopes are resolved against the active security context. |
| Object-specific rules | Table, column, domain, and protected-material rules all apply where their scopes intersect. |
| Hidden metadata | Metadata existence and expression details can be hidden or redacted independently of row access. |

When composition is ambiguous, the operation must fail closed. The engine must not infer allow behavior from missing policy rows.

## Visibility Examples

Tenant isolation:

```sql
create rls app.orders_tenant_rls
on table app.orders
for select, update, delete
to role app_user
using (tenant_uuid = current_tenant_uuid())
with check (tenant_uuid = current_tenant_uuid())
active;
```

Support role with masked values:

```sql
grant select on table app.customer to app_support;
grant select on column app.customer.customer_id to app_support;
grant select on column app.customer.display_name to app_support;
grant select on column app.customer.email to app_support;

create mask app.customer_email_mask
on column app.customer.email
using case
  when has_role('app_support_private') then email
  else 'redacted'
end
to role app_support
active;
```

Read/write separation:

```sql
create rls app.invoice_read_rls
on table app.invoice
for select
to role app_billing_reader
using (tenant_uuid = current_tenant_uuid())
active;

create rls app.invoice_write_rls
on table app.invoice
for insert, update
to role app_billing_writer
with check (
  tenant_uuid = current_tenant_uuid()
  and invoice_state in ('draft', 'review')
)
active;
```

## SHOW And DESCRIBE

Security inspection is itself security-sensitive.

```sql
show policies;
show masks;
show rls;
describe policy app.orders_support_visibility;
describe mask app.customer_email_mask;
describe rls app.orders_tenant_rls;
```

Inspection output may include:

| Field | Disclosure rule |
| --- | --- |
| Object UUID | Shown only when the caller can inspect stable identity. |
| Display name | Shown when metadata visibility allows it. |
| Target binding | Redacted or hidden when the target is outside the caller's metadata scope. |
| Principal scope | Redacted when role, group, user, or agent membership is protected. |
| Predicate descriptor | May show expression shape without protected constants. |
| Mask expression | Often redacted; raw expression display requires explicit authority. |
| Enforcement state | Usually visible to administrators; may be summarized for ordinary users. |
| Security epoch | Administrative diagnostic field. |
| Validation state | Shows active, inactive, invalid, stale, recovery-fenced, or policy-defined readiness. |

`SHOW MASKS` and `SHOW RLS` route through security inspection and must return only authorized policy metadata. They do not expose stored protected values.

## Dependency And Invalidation

Policies, masks, and RLS rules create dependency edges.

| Dependency | Why it matters |
| --- | --- |
| Target object | Dropping or renaming a target can invalidate the rule. |
| Column or domain descriptor | Type changes can invalidate predicates or mask expressions. |
| Function or routine | A predicate or mask expression can depend on callable behavior. |
| Principal | Dropping or disabling a user, role, group, or agent can invalidate the scope. |
| Protected material | Release, backup, support, and diagnostic policy can depend on protected material identity. |
| View or materialized view | A rule can attach to the view, base rowset, or both according to binding policy. |

After a policy change, ScratchBird must invalidate dependent:

- prepared statements;
- query plans and optimizer evidence;
- parser metadata caches;
- driver metadata;
- catalog projections;
- support-bundle manifests;
- diagnostic renderers;
- security snapshots;
- view and materialized-view readiness where applicable.

## Transactions And Recovery

Policy lifecycle changes are transactional catalog/security mutations.

| Event | Contract |
| --- | --- |
| Create or alter before commit | Visible only inside the owning transaction where the engine admits it. |
| Commit | Advances the visible catalog/security epoch and invalidates dependent state. |
| Rollback | Restores the prior visible policy state. |
| Crash before commit | Must recover to the old committed policy state or a recovery-required fail-closed state. |
| Crash after commit | Must recover to the committed policy state or fail closed if certainty is impossible. |
| Recovery uncertainty | Protected access is refused or redacted until operator or recovery policy resolves it. |

Policy state never becomes final because a parser accepted text or because a generated envelope exists. MGA transaction finality and engine recovery decide visibility.

## Interaction With Views And Catalog Projections

Views do not bypass table policy. A view can have its own policy, and its base objects can also have policies. The final result is the intersection of admitted visibility, column grants, row rules, masks, protected-material policy, and the view definition.

Catalog projections may show metadata outside a user's ordinary sandbox only when the projection itself has authority to do so. A grant on a projection is not a grant on the hidden base catalog rows.

Materialized views must record whether their stored rows already include policy-filtered data, whether refresh runs as invoker or definer, and whether a read from the materialized view rechecks caller policy. If that state is ambiguous, refresh or read access must fail closed.

## Interaction With Domains And Protected Material

Domains can carry masking, validation, cast, operation, null, and element policies. A table column using such a domain inherits the domain policy where the descriptor says it applies.

Protected material requires release policy. Ordinary `SELECT`, `SHOW`, `DESCRIBE`, diagnostics, and support bundles must not reveal raw protected values unless release policy explicitly admits that route for the effective principal and purpose.

```sql
select *
from sys.catalog.protected_material_policy_binding
limit 20;
```

That query reads an authorized catalog projection. It does not grant access to protected material itself.

## Error And Refusal Cases

Common fail-closed cases include:

| Case | Expected behavior |
| --- | --- |
| Unknown policy, mask, RLS, target, or principal | Refuse with a name-resolution diagnostic that does not reveal hidden objects. |
| Ambiguous target or principal | Refuse with an ambiguity diagnostic where disclosure policy allows it. |
| Unauthorized lifecycle change | Refuse with a security diagnostic. |
| Unsupported option | Refuse; do not store unknown options as inert text. |
| Predicate type is not boolean | Refuse with a descriptor diagnostic. |
| Mask expression type is incompatible | Refuse with a descriptor diagnostic. |
| Target descriptor changed | Revalidate or refuse until the rule is corrected. |
| Rule would fail open | Refuse the lifecycle operation or quarantine the rule. |
| Policy state is stale during execution | Rebind or refuse. |
| Recovery uncertainty | Refuse or redact protected access until recovery is certain. |
| Diagnostics would leak protected values | Return redacted diagnostics or a generic refusal vector. |

## Public Proof Expectations

A complete proof suite for this surface should include:

- parser acceptance and canonical refusal for policy, mask, and RLS lifecycle statements;
- UUID-bound lowering for policy/mask/RLS names, target objects, principal scopes, predicates, and mask expressions;
- SBLR admission for security mutation and inspection routes;
- engine dispatch proof for create, alter, show, describe, validate, and drop routes;
- transaction proof for commit, rollback, crash/reopen, and security epoch advancement;
- privilege proof that unauthorized users cannot create, alter, drop, inspect, or apply policies;
- row-result proof that RLS filters `SELECT`, `UPDATE`, `DELETE`, `MERGE`, and UPSERT targets correctly;
- mutation proof that `WITH CHECK` validates inserted and updated row images;
- mask proof that stored values remain unchanged while projected values are transformed;
- support-bundle and diagnostic proof that protected material and masked values do not leak;
- cache invalidation proof for prepared statements, plans, metadata, and catalog projections;
- fuzz proof for malformed predicates, oversized expressions, hidden names, stale UUIDs, and invalid descriptors.

The visible public route evidence includes `CREATE POLICY ... ON TABLE ...`, `ALTER POLICY ... SET INACTIVE`, `SHOW POLICIES`, `SHOW MASKS`, and `SHOW RLS` route fixtures. Those fixtures prove bounded parser binding, SBLR admission, server dispatch, and security inspection/mutation routing for the public surface. Broader semantic proof still belongs in normal project tests as specific RLS, mask, mutation, diagnostic, and recovery gates.

## Verification Checklist

| Check | Required outcome |
| --- | --- |
| Parse | Statement shape is recognized contextually by SBsql. |
| Bind | Names, UUIDs, descriptors, target scopes, principal scopes, predicates, and options resolve exactly. |
| Authorize | The effective user or agent UUID can perform the requested lifecycle or inspection operation. |
| Admit | SBLR route and result shape are accepted by the engine verifier. |
| Execute | Security mutation or inspection dispatches through engine-owned APIs. |
| Commit | Catalog/security state becomes visible only through MGA transaction finality. |
| Invalidate | Dependent plans, metadata, security snapshots, diagnostics, and support projections are refreshed or refused. |
| Enforce | Reads, writes, metadata, diagnostics, and support bundles apply row policy, masks, and protected-material rules. |
| Recover | Crash and reopen cannot produce silent fail-open access. |
