# Security And Sandboxing

This page is part of the SBsql Language Reference Manual. It explains the
security model that applies before SBsql text can become engine work: identity,
roles, grants, sandbox roots, catalog visibility, policy, masking, row-level
security, protected material, and fail-closed refusal behavior.

Generation task: `core_paradigms_security_and_sandboxing`

## Purpose

Security in ScratchBird is materialized engine state, not a gate checked once
at login. Every operation is evaluated against the intersection of who the
caller is, what roles they hold, which schema sandbox they are in, what policy
applies to the object they are touching, and what the current transaction and
recovery state permits. Passing authentication, owning an object, or holding a
GRANT are all necessary inputs — but the engine rechecks all of them at
execution time, not just at the moment the session opened.

The model is fail-closed: when any required piece of evidence is missing,
stale, ambiguous, or contradicted by an explicit denial, the engine refuses the
operation rather than guessing:

- explicit denial wins over allow;
- hidden objects must stay hidden unless disclosure policy admits them;
- policy, masks, and row-level security narrow access after ordinary grants;
- server-local file, network, bridge, stream, backup, restore, diagnostic, and
  management surfaces require explicit policy admission;
- missing, stale, ambiguous, corrupted, or recovery-fenced security evidence
  refuses the operation rather than silently allowing it.

## Security Flow

![diagram](./security_and_sandboxing-1.svg)

Security is evaluated on bound identity. A name that cannot be resolved inside
the session's visible schema branch does not become a security decision about a
hidden object. When disclosure policy requires it, missing-object and
hidden-object diagnostics can be intentionally indistinguishable.

## Principal Model

Every principal has durable UUID identity. Names are resolver inputs and display
labels.

| Principal | Purpose |
| --- | --- |
| User | Human or application identity that can attach, authenticate, own objects, and receive grants. |
| Agent | Admitted engine, management, maintenance, migration, replication, or operational actor with a UUID and policy-bound authority. |
| Role | Privilege bundle that can be activated by a session where policy admits it. |
| Group | Membership collection used to organize users, agents, and roles. |
| Public | Optional pseudo-principal for privileges intentionally available to every attached session. |
| Owner | Principal recorded as owning a durable object. Ownership is materialized authorization, not a parser bypass. |

The effective security context for a statement includes:

- authenticated principal UUID;
- attached database or workarea UUID;
- sandbox root;
- home schema and current schema;
- active role set;
- group membership;
- session attributes admitted by policy;
- transaction UUID and isolation profile;
- security epoch;
- policy epoch;
- redaction and disclosure profile;
- operation descriptor.

## Authentication And Session Binding

Authentication proves that a client or agent may become a principal in the
current attach context. Authorization decides what that principal may do after
attach.

An attach can establish:

- user UUID;
- agent UUID;
- provider identity evidence;
- session UUID;
- attached database/workarea root;
- network, IPC, embedded, or bridge endpoint evidence;
- default schema and home schema;
- initial active role set;
- sandbox root;
- policy profile;
- resource limits and timeout profile.

Raw secret material is not ordinary SBsql data. Secret references, provider
tokens, credential handles, and protected-material handles must not be rendered
in result sets, logs, diagnostics, catalog display, support bundles, or message
vectors unless an explicit protected-material release policy admits that
specific disclosure.

## Sandboxed Schema Roots

A sandbox root limits what a session can name, inspect, and operate on through
ordinary name resolution. A session connected to a workarea, tenant branch,
application branch, or other bounded schema root sees that root as its visible
world unless SBsql administrative authority and policy admit broader access.

![diagram](./security_and_sandboxing-2.svg)

Sandbox rules:

- ordinary name resolution starts inside the session root;
- recursive schema lookup cannot escape the admitted root;
- current schema and search path entries outside the root are refused or hidden;
- grants inside the sandbox do not imply grants outside the sandbox;
- catalog projections can render authorized metadata from outside the root only
  when the projection object itself has authority and policy admits disclosure;
- support diagnostics must redact names, UUIDs, paths, values, and policy
  details according to the session's disclosure profile.

See [Schema Tree And Name Resolution](../syntax_reference/schema_tree_and_name_resolution.md).

## Grants And Effective Privileges

Grants attach privileges to principal UUIDs, roles, groups, or public scope.
Revokes remove privilege edges. Effective privileges are computed for a bound
operation, not for raw source text.

Resolution order:

1. Authenticate or bind the effective principal UUID.
2. Establish sandbox root, attached database/workarea, current schema, and home
   schema.
3. Resolve the target object to UUID identity under metadata visibility rules.
4. Collect direct grants to the principal.
5. Collect grants inherited through active roles and admitted groups.
6. Apply ownership and administration policy.
7. Apply explicit denials.
8. Apply row, column, object, stream, file, bridge, and management policies.
9. Check transaction and recovery gates.
10. Admit or refuse the operation.

Common privilege targets include:

| Target | Examples Of Controlled Operations |
| --- | --- |
| Database | connect, create schema, create filespace, backup, restore, security administration, configuration. |
| Schema | usage, create object, alter, drop, describe. |
| Table | select, insert, update, delete, truncate, references, trigger, alter, drop, comment, describe. |
| Column | select, insert, update, references, describe. |
| View and materialized view | select, refresh, alter, drop, comment, describe. |
| Routine | execute, alter, drop, comment, describe. |
| Domain and type descriptor | usage, alter, drop, comment, describe. |
| Sequence | usage, select, update, alter, drop. |
| Policy, mask, and RLS | apply, alter, drop, enable, disable, describe, validate. |
| Filespace | usage, create object, attach, detach, alter, drop, describe, promote where admitted. |
| Stream or bridge | connect, import, export, replicate, migrate, validate, cutover. |
| Management surface | show metrics, manage sessions, validate configuration, generate support bundle, run admitted maintenance. |

See [Security And Privilege Statements](../syntax_reference/security_and_privilege_statements.md).

## Explicit Deny Wins

An allow edge can make an operation possible. A deny edge or policy refusal can
still block it.

Examples:

- a user has `SELECT` on a table, but RLS hides rows for that user's tenant;
- a user has `SELECT` on a column, but a mask returns a redacted value;
- a user has `UPDATE` on a table, but a policy denies updates after a workflow
  state is sealed;
- a user has a management role, but a recovery fence denies write admission;
- a user can create objects in a schema, but filespace policy denies the
  requested placement.

Denial is not a parser error. It is an explicit message vector result.

## Policy, Masks, And Row-Level Security

Policy objects refine grant authority.

| Object | Role |
| --- | --- |
| Policy | General durable rule applied to an object, operation family, principal scope, security context, stream, bridge, support surface, or management surface. |
| Mask | Value rendering rule that transforms or redacts a visible value before it leaves the engine. |
| RLS | Row-level rule that filters visible row versions or checks proposed row mutations. |

Evaluation order is conceptually:

1. MGA determines which row versions are visible to the transaction.
2. Object and column privileges determine whether the operation can proceed.
3. RLS filters visible rows or refuses row mutation.
4. Masks transform values that are visible but protected.
5. Protected-material release policy decides whether sensitive values can leave
   the engine.
6. Diagnostics and support output are redacted according to disclosure policy.

RLS is not a transaction system. It does not decide whether a row version exists
or whether a transaction committed. MGA owns that. RLS decides whether the
effective security context may see or mutate the otherwise visible row.

See [Policy, Mask, And RLS Lifecycle](../syntax_reference/policy_mask_and_rls.md).

## Protected Material

Protected material includes secrets, credentials, keys, tokens, protected
configuration values, masked values, sensitive diagnostic fields, and any value
whose release is governed by policy.

Rules:

- store and pass secret references rather than raw secrets where possible;
- redact protected material in diagnostics and support bundles by default;
- do not expose protected values through casts, string concatenation, errors,
  logs, `show`, `describe`, catalog views, or procedure output without release
  authority;
- treat export, backup, replication, migration, bridge, and stream routes as
  protected-material release surfaces when values can cross a boundary;
- record audit evidence for admitted release where policy requires it.

## Catalog Projections

Catalog tables and views can reveal metadata. Metadata itself can be sensitive:
object names, existence, ownership, privilege edges, policy names, filespace
layout, endpoint labels, and diagnostic handles can all disclose information.

ScratchBird therefore distinguishes:

- base catalog authority;
- projection authority;
- disclosure policy;
- redaction policy;
- sandbox root;
- ordinary object privileges.

A user may have `SELECT` on an authorized catalog projection that renders a
safe view of objects outside the sandbox. That does not grant the user direct
name resolution or object privileges outside the sandbox.

## Procedural Security

Procedures, functions, packages, and triggers run with an explicit security
mode. The mode is part of the routine descriptor and is checked when the routine
is compiled, invoked, invalidated, or revalidated.

| Mode | Contract |
| --- | --- |
| Invoker rights | The routine executes with the caller's effective principal, active roles, sandbox root, and policy context. |
| Definer rights | The routine executes with admitted definer authority for the routine body while preserving caller context for auditing and policy inputs. |
| Agent rights | The routine is invoked by an admitted agent and runs only within the agent's registered authority and purpose. |
| Restricted rights | The routine runs with an intentionally reduced privilege set even when caller or definer has broader authority. |

Procedural security rules:

- routine source text is not authority;
- compiled routine SBLR must reference UUID-bound objects and descriptors;
- dependency, grant, policy, domain, type, and catalog changes can invalidate a
  compiled routine;
- dynamic execution must pass through parse, bind, admission, authorization,
  and engine dispatch;
- cursors, result sets, streams, and protected values passed between routines
  retain descriptor and security context;
- trigger execution must not bypass the security and transaction context of the
  firing operation.

See [Procedural SQL](../syntax_reference/procedural_sql.md),
[Function Lifecycle](../syntax_reference/function.md),
[Procedure Lifecycle](../syntax_reference/procedure.md), and
[Trigger Lifecycle](../syntax_reference/trigger.md).

## File, Stream, Bridge, And Network Gates

Operations that move data across a boundary are policy-controlled even when the
ordinary object privileges are present.

| Surface | Security Rule |
| --- | --- |
| `COPY FROM STDIN` | Client-supplied stream frames are admitted through the stream contract and target-object privileges. |
| `COPY TO STDOUT` | Data can leave the engine only through result and stream policy. |
| Server-local location | Opening a server-local path requires explicit policy admission before any file access occurs. |
| Logical backup | May stream logical data only through admitted backup and protected-material policy. |
| Logical restore | May ingest logical instructions only through admitted restore policy and target privileges. |
| Replication and CDC | Requires source, target, ordering, idempotency, quarantine, and cutover authority. |
| Migration | Requires metadata, data, stream, validation, and cutover authority. |
| Bridge | Requires bridge-use privilege, endpoint policy, identity delegation policy, and stream policy. |
| Management diagnostics | Requires management privilege and redaction policy. |

Low-level repair, verification, page-copy backup, page-copy restore, and direct
storage manipulation are not ordinary parser privileges. They require explicit
administrative SBsql routes and policy admission.

See [COPY Streaming Import And Export](../syntax_reference/copy.md) and
[Backup, Restore, Replication, And Migration](../syntax_reference/backup_restore_replication_migration.md).

## Agent Sandboxing

Agents are principals. They are not ambient superusers.

An agent has:

- UUID identity;
- registered purpose;
- owner or controller;
- admitted scope;
- allowed operation families;
- resource limits;
- activation policy;
- audit policy;
- shutdown and cancellation policy;
- support-bundle disclosure policy.

An agent may run maintenance, migration, replication, validation, support, or
management work only within its registered authority. Agent work still uses
SBLR admission, engine authorization, MGA transaction authority, resource
limits, and fail-closed recovery behavior.

See [Agents And Agent Management](../syntax_reference/agent.md).

## Security Epochs And Invalidation

Security changes advance a security epoch. Dependent state must be invalidated
or revalidated when the epoch changes.

Dependent state includes:

- prepared statements;
- bound SBLR envelopes;
- compiled procedures and functions;
- trigger plans;
- optimizer plans and plan cache entries;
- metadata caches;
- catalog projection caches;
- bridge sessions;
- stream authorizations;
- support-bundle projections;
- active security snapshots where policy requires revalidation.

Invalidation prevents stale authorization from surviving a grant, revoke,
policy, mask, role, group, identity, sandbox, or protected-material change.

## Statement Examples

Create a role, grant schema usage, and grant read access:

```sql
create role app_reader;

grant usage on schema app to app_reader;
grant select on table app.orders to app_reader;
```

Activate a role for the current session:

```sql
set role app_reader;
```

Create a row-level rule that narrows visible rows:

```sql
create rls app.orders_tenant_rls
on table app.orders
for select
using tenant_uuid = current_tenant_uuid()
to role app_reader
active;
```

Create a mask for a protected column:

```sql
create mask app.customer_email_mask
on column app.customer.email
using case
    when has_role('support_full_contact') then email
    else null
end
active;
```

Attempting to read outside the sandbox returns a denial or a redacted
missing-object diagnostic according to disclosure policy:

```sql
select *
from admin.security_audit;
```

## Syntax Productions

```ebnf
dcl_security_stmt ::=
      create_principal_stmt
    | alter_principal_stmt
    | drop_principal_stmt
    | grant_stmt
    | revoke_stmt
    | set_role_statement
    | show_security_statement
    | describe_security_statement ;
```

```ebnf
principal_ref           ::= uuid_reference
                          | qualified_name ;
```

```ebnf
grant_stmt              ::= "GRANT" grant_payload "TO" principal_ref grant_option_list? ;
```

```ebnf
revoke_stmt             ::= "REVOKE" revoke_payload "FROM" principal_ref revoke_option_list? ;
```

```ebnf
policy_stmt             ::= create_policy_statement
                          | alter_policy_statement
                          | drop_policy_statement
                          | create_mask_statement
                          | alter_mask_statement
                          | drop_mask_statement
                          | create_rls_statement
                          | alter_rls_statement
                          | drop_rls_statement
                          | show_policy_statement
                          | describe_policy_statement
                          | validate_policy_statement ;
```

## Binding And Execution Summary

| Step | Security Meaning |
| --- | --- |
| Parse | The statement shape is recognized. No authority is granted. |
| Bind principal | User, role, group, agent, or public references resolve to UUIDs where visible. |
| Bind target | Object names resolve under sandbox and metadata visibility rules. |
| Bind descriptors | Parameters, expressions, rowsets, streams, masks, policies, and result shapes receive descriptors. |
| Admit envelope | Server admission checks route, envelope version, operation identity, and gated capability state. |
| Authorize | Grants, roles, groups, ownership, deny edges, policy, masks, RLS, and protected-material rules are evaluated. |
| Execute | Engine performs the admitted operation under MGA and recovery authority. |
| Render | Results and diagnostics are redacted according to disclosure policy. |

## Refusal Classes

| Refusal | Typical Cause |
| --- | --- |
| `unsupported` | Security surface, grant target, policy option, provider route, or management route is not available in the build or target. |
| `denied` | Principal lacks privilege, sandbox blocks resolution, policy blocks access, protected material cannot be released, recovery fences work, or server-local access is not admitted. |
| `unlicensed` | A recognized route reaches a provider or gated boundary that reports the capability is not licensed. |
| Parse error | The source text does not form a valid SBsql statement. |
| Bind error | Principal, object, descriptor, role, policy, or target reference cannot be resolved unambiguously. |

See [Refusal Vectors](../syntax_reference/refusal_vectors.md).

## Verification Checklist

A security and sandbox proof should demonstrate:

- authenticated sessions bind to a principal UUID;
- unauthenticated sessions cannot perform protected operations;
- role activation changes effective privileges only where admitted;
- explicit deny overrides direct and inherited allow;
- sandbox roots prevent ordinary name resolution outside the root;
- hidden objects do not leak through diagnostics when disclosure policy forbids
  it;
- authorized catalog projections can render only policy-admitted metadata;
- grants and revokes are transactional;
- security epoch changes invalidate dependent plans and metadata;
- RLS filters otherwise visible row versions;
- masks transform visible values without changing stored values;
- protected material is redacted from diagnostics and support output by default;
- server-local file access is denied unless explicitly admitted;
- bridge and stream operations require endpoint, identity, object, and stream
  authority;
- agent operations run only inside registered authority;
- recovery-required state fences unsafe security and data operations;
- unsupported, denied, and unlicensed routes return explicit message vectors.

## Related Reference Pages

- [Intro And MGA](intro_and_mga.md)
- [Parser To SBLR Pipeline](parser_to_sblr_pipeline.md)
- [UUID Catalog Identity](uuid_catalog_identity.md)
- [Transactions And Recovery](transactions_and_recovery.md)
- [Security And Privilege Statements](../syntax_reference/security_and_privilege_statements.md)
- [Policy, Mask, And RLS Lifecycle](../syntax_reference/policy_mask_and_rls.md)
- [Schema Tree And Name Resolution](../syntax_reference/schema_tree_and_name_resolution.md)
- [Refusal Vectors](../syntax_reference/refusal_vectors.md)
- [Agents And Agent Management](../syntax_reference/agent.md)
- [COPY Streaming Import And Export](../syntax_reference/copy.md)
- [Backup, Restore, Replication, And Migration](../syntax_reference/backup_restore_replication_migration.md)
