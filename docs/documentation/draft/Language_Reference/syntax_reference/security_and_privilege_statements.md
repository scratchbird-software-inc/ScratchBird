# Security And Privilege Statements

This page is part of the SBsql Language Reference Manual. It documents identities, roles, groups, grants, revokes, privilege resolution, sandboxing, and security inspection surfaces for SBsql.

Generation task: `syntax_reference_security_privilege`

Related pages: [Security And Sandboxing](../core_paradigms/security_and_sandboxing.md), [Policy, Mask, And RLS Lifecycle](policy_mask_and_rls.md), [Schema Tree And Name Resolution](schema_tree_and_name_resolution.md), [Table Lifecycle](table.md), [View Lifecycle](view.md), [Procedure Lifecycle](procedure.md), [Function Lifecycle](function.md), and [Refusal Vectors](refusal_vectors.md).

## Purpose

Security statements create and mutate durable authorization state. They do not grant authority merely because the parser accepts text. The effective user or agent UUID, active role set, group membership, sandbox root, security policy snapshot, object UUID, and operation descriptor decide whether a statement is admitted.

The security model is fail-closed:

- explicit denial wins over allow;
- hidden objects must not be revealed by error detail unless policy allows disclosure;
- grants on catalog projections do not grant authority on hidden base catalog rows;
- grants inside a sandbox do not allow name resolution outside the sandbox root;
- revocation and policy changes invalidate prepared statements, metadata caches, support projections, and active security snapshots that depend on them.

## Principal Model

| Principal | Meaning |
| --- | --- |
| User | Human or application identity that can authenticate or be delegated by an admitted provider. |
| Agent | Engine or management actor identity represented by a UUID and admitted by policy. |
| Role | Named privilege bundle that can be granted to users, groups, agents, or other roles where policy admits nesting. |
| Group | Membership collection used for identity organization and inherited authorization. |
| Public | Optional database-wide pseudo-principal for privileges intentionally granted to every attached session. |
| Owner | Object owner UUID recorded in catalog metadata. Ownership is materialized authorization, not a parser bypass. |

Every principal has a durable UUID. Display names are resolver input and may be hidden, localized, renamed, or policy-rendered.

## Lifecycle Statement Surface

| Operation | Surface | Contract |
| --- | --- | --- |
| Create identity | `CREATE USER`, `CREATE ROLE`, `CREATE GROUP` | Creates a durable principal UUID with provider, status, default schema, home schema, and metadata policy. |
| Alter identity | `ALTER USER`, `ALTER ROLE`, `ALTER GROUP` | Changes admitted identity metadata, status, authentication provider binding, membership policy, or defaults. |
| Drop identity | `DROP USER`, `DROP ROLE`, `DROP GROUP` | Retires the principal only when ownership, grants, dependencies, and audit requirements are handled. |
| Grant privilege | `GRANT ... ON ... TO ...` | Adds an allow edge from a privilege payload to one or more principals. |
| Grant role | `GRANT role_name TO principal` | Adds a role-membership edge, optionally with administration authority. |
| Revoke privilege | `REVOKE ... ON ... FROM ...` | Removes an allow edge, or removes only grant-option authority when requested. |
| Revoke role | `REVOKE role_name FROM principal` | Removes role membership or role administration authority. |
| Set role | `SET ROLE ...`, `RESET ROLE` | Changes the active role set for the session where policy admits it. |
| Show security | `SHOW GRANTS`, `SHOW PRIVILEGES`, `SHOW ROLES`, `SHOW USERS`, `SHOW GROUPS` | Returns authorized security projections only. |
| Describe security | `DESCRIBE USER`, `DESCRIBE ROLE`, `DESCRIBE GROUP`, `DESCRIBE GRANT` | Returns one security object's metadata according to disclosure policy. |

Policy, masking, and row-level security lifecycle statements are documented in [Policy, Mask, And RLS Lifecycle](policy_mask_and_rls.md).

## Syntax

```ebnf
security_statement ::=
      create_identity_statement
    | alter_identity_statement
    | drop_identity_statement
    | grant_statement
    | revoke_statement
    | set_role_statement
    | show_security_statement
    | describe_security_statement ;
```

```ebnf
create_identity_statement ::=
    CREATE (USER | ROLE | GROUP) principal_name identity_option* ;

alter_identity_statement ::=
    ALTER (USER | ROLE | GROUP) principal_ref alter_identity_action+ ;

drop_identity_statement ::=
    DROP (USER | ROLE | GROUP) principal_ref (RESTRICT | CASCADE)? ;
```

```ebnf
grant_statement ::=
      GRANT privilege_list ON grant_target TO grantee_list grant_option*
    | GRANT role_list TO grantee_list role_grant_option* ;

revoke_statement ::=
      REVOKE revoke_option* privilege_list ON grant_target FROM grantee_list revoke_behavior?
    | REVOKE revoke_option* role_list FROM grantee_list revoke_behavior? ;
```

```ebnf
privilege_list ::=
      ALL PRIVILEGES
    | privilege_name ("," privilege_name)* ;

grant_target ::=
      DATABASE qualified_name
    | SCHEMA qualified_name
    | TABLE qualified_name
    | VIEW qualified_name
    | MATERIALIZED VIEW qualified_name
    | COLUMN qualified_name "." identifier
    | SEQUENCE qualified_name
    | DOMAIN qualified_name
    | TYPE DESCRIPTOR qualified_name
    | FUNCTION qualified_name
    | PROCEDURE qualified_name
    | TRIGGER qualified_name
    | POLICY qualified_name
    | MASK qualified_name
    | RLS qualified_name
    | FILESPACE qualified_name
    | BRIDGE qualified_name
    | SYSTEM qualified_name ;

grant_option ::=
      WITH GRANT OPTION
    | AS principal_ref ;

role_grant_option ::=
      WITH ADMIN OPTION
    | AS principal_ref ;

revoke_option ::=
      GRANT OPTION FOR
    | ADMIN OPTION FOR ;

revoke_behavior ::=
      RESTRICT
    | CASCADE ;
```

```ebnf
set_role_statement ::=
      SET ROLE role_ref
    | SET ROLE NONE
    | RESET ROLE ;

show_security_statement ::=
      SHOW GRANTS (FOR principal_ref)?
    | SHOW PRIVILEGES ON grant_target
    | SHOW ROLES
    | SHOW USERS
    | SHOW GROUPS ;
```

SBsql is context sensitive. These words are contextual command words in this statement family; they do not need to be globally reserved in unrelated expression positions.

## Identity Statements

Identity statements bind to principal UUIDs. They may render names, provider labels, status, default schema, or home schema, but secret material must not be exposed in ordinary statement text, diagnostics, metadata views, logs, or support bundles.

| Option Area | Contract |
| --- | --- |
| Authentication provider | Binds the principal to a provider or provider group admitted by database policy. |
| Secret material | Uses secret references or provider-owned credentials. Raw secrets are not catalog display values. |
| Status | Active, disabled, expired, locked, or policy-defined states affect attach and authorization. |
| Default schema | Initial current schema or search root for ordinary sessions. |
| Home schema | User-owned schema root selected by identity policy. |
| Group membership | May be changed through group alteration or role/group grants, subject to policy. |
| Owner reassignment | Dropping an identity that owns objects requires explicit reassignment, cascade, or refusal. |

Example:

```sql
create role app_reader;
create group app_support;
create user app_reporter;
```

## Privilege Classes

The exact admitted privileges for an object are determined by object class and policy. The public SBsql privilege families are:

| Object Class | Common Privileges |
| --- | --- |
| Database | `CONNECT`, `CREATE SCHEMA`, `CREATE USER`, `CREATE ROLE`, `CREATE GROUP`, `CREATE FILESPACE`, `ALTER`, `DROP`, `BACKUP`, `RESTORE`, `MANAGE SECURITY`, `MANAGE CONFIGURATION` |
| Schema | `USAGE`, `CREATE TABLE`, `CREATE VIEW`, `CREATE FUNCTION`, `CREATE PROCEDURE`, `CREATE TRIGGER`, `CREATE DOMAIN`, `CREATE SEQUENCE`, `ALTER`, `DROP` |
| Table | `SELECT`, `INSERT`, `UPDATE`, `DELETE`, `TRUNCATE`, `REFERENCES`, `TRIGGER`, `ALTER`, `DROP`, `COMMENT`, `DESCRIBE` |
| Column | `SELECT`, `INSERT`, `UPDATE`, `REFERENCES`, `DESCRIBE` |
| View | `SELECT`, `ALTER`, `DROP`, `COMMENT`, `DESCRIBE` |
| Materialized view | `SELECT`, `REFRESH`, `ALTER`, `DROP`, `COMMENT`, `DESCRIBE` |
| Sequence | `USAGE`, `SELECT`, `UPDATE`, `ALTER`, `DROP` |
| Function | `EXECUTE`, `ALTER`, `DROP`, `COMMENT`, `DESCRIBE` |
| Procedure | `EXECUTE`, `ALTER`, `DROP`, `COMMENT`, `DESCRIBE` |
| Trigger | `ALTER`, `DROP`, `ENABLE`, `DISABLE`, `DESCRIBE` |
| Domain | `USAGE`, `ALTER`, `DROP`, `COMMENT`, `DESCRIBE` |
| Type descriptor | `USAGE`, `ALTER`, `DROP`, `DESCRIBE` |
| Policy, mask, RLS | `APPLY`, `ALTER`, `DROP`, `ENABLE`, `DISABLE`, `DESCRIBE` |
| Filespace | `USAGE`, `CREATE OBJECT`, `ALTER`, `DROP`, `DESCRIBE` |
| Bridge | `CONNECT`, `IMPORT`, `EXPORT`, `REPLICATE`, `ALTER`, `DROP`, `DESCRIBE` |
| System or management surface | Policy-defined administrative privileges such as `SHOW METRICS`, `MANAGE SESSIONS`, `VALIDATE CONFIGURATION`, or `SUPPORT BUNDLE` |

`ALL PRIVILEGES` means all grantable privileges for the specified target class that the grantor is authorized to grant. It does not include privileges outside that target class.

## Grant Targets

A grant target binds to an object UUID, not to display text alone.

| Target Form | Binding Rule |
| --- | --- |
| `DATABASE name` | Binds to a database catalog identity. |
| `SCHEMA name` | Binds to a schema UUID. Does not grant child-object privileges unless policy explicitly defines inherited behavior. |
| `TABLE name` | Binds to a table UUID. Column restrictions are independent. |
| `COLUMN table.column` | Binds to a column descriptor within a table UUID. |
| `VIEW name` | Binds to a view UUID. Underlying table privileges are not automatically granted. |
| `MATERIALIZED VIEW name` | Binds to a materialized-view UUID and its refresh/read privileges. |
| `FUNCTION name`, `PROCEDURE name` | Bind to callable routine identity and argument descriptor where needed. |
| `DOMAIN name` | Binds to a domain UUID. Grants usage or administration of that domain, not arbitrary access to stored values. |
| `BRIDGE name` | Binds to a bridge connection descriptor. External access still requires policy and session authority. |
| `SYSTEM name` | Binds to a named administrative surface. System grants should be narrow and auditable. |

If a name is hidden by metadata policy, the resolver may return the same user-facing diagnostic as a missing object.

## Grant Option And Admin Option

`WITH GRANT OPTION` allows the grantee to grant the same object privilege to another principal, subject to sandbox and policy checks.

`WITH ADMIN OPTION` allows the grantee to administer a role membership edge, subject to role policy.

Grantable authority is never broader than the grantor's effective authority. A user cannot grant a privilege merely because they can spell the object or role name.

Example:

```sql
grant usage on schema app to app_reader;
grant select on table app.orders to app_reader with grant option;
grant app_reader to app_reporter;
grant app_reader to app_support with admin option;
```

## Revoke Semantics

`REVOKE` removes grant edges. It does not delete objects or erase audit history.

| Form | Effect |
| --- | --- |
| `REVOKE privilege ON target FROM principal` | Removes the privilege edge from that principal. |
| `REVOKE GRANT OPTION FOR privilege ON target FROM principal` | Removes the ability to re-grant while preserving the privilege itself where admitted. |
| `REVOKE role FROM principal` | Removes role membership. |
| `REVOKE ADMIN OPTION FOR role FROM principal` | Removes role administration authority while preserving membership where admitted. |
| `RESTRICT` | Refuses if dependent grants, role memberships, or active policy edges would become invalid. |
| `CASCADE` | Removes dependent grant edges through an explicit cascade plan and audit record. |

Example:

```sql
revoke grant option for select on table app.orders from app_reader restrict;
revoke select on table app.orders from app_reader cascade;
revoke admin option for app_reader from app_support restrict;
```

Revocation takes effect transactionally. A rollback restores the prior visible grant state. A commit advances the security epoch and invalidates dependent execution state.

## Effective Privilege Resolution

For each protected operation, the engine resolves effective privileges in this order:

1. Authenticate the session or agent and bind the effective principal UUID.
2. Establish sandbox root, attached database/workarea, current schema, and active role set.
3. Resolve the target name or UUID under the sandbox and metadata visibility rules.
4. Collect direct grants to the principal.
5. Collect grants inherited through active roles and admitted groups.
6. Apply object ownership and administration policy.
7. Apply object-class privilege rules.
8. Apply column, element, row-level, mask, protected-material, bridge, and system policies.
9. Apply explicit deny or refusal policy. Denial wins over allow.
10. Produce an admitted operation descriptor or a canonical refusal message vector.

Authorization is rechecked at execution. A prepared statement that was valid when prepared can be refused later if security state, schema state, policy state, or object state changed.

## Object Ownership

Object ownership is represented in catalog metadata and materialized security state. Ownership may imply administrative authority only where policy says so.

| Concern | Contract |
| --- | --- |
| Owner name | Display metadata only; owner UUID is authority. |
| Owner transfer | Must be explicit, authorized, audited, and dependency-aware. |
| Owner drop | Dropping an owner principal requires reassignment, cascade, or refusal. |
| Owner bypass | Ownership is not a bypass for explicit denial, sandbox boundaries, protected material, or fail-closed recovery. |

## Column, Row, And Mask Interaction

`GRANT SELECT ON TABLE` admits table read only if column, row, and mask policy also admit the specific projection.

| Security Layer | Effect |
| --- | --- |
| Table privilege | Allows access to the relation as a target. |
| Column privilege | Allows individual column projection or mutation. |
| Row-level security | Filters or refuses rows according to policy. |
| Mask policy | Rewrites visible values according to masking policy. |
| Protected material | Requires protected-value release policy; ordinary SELECT does not expose raw secrets. |
| Catalog projection | May show metadata from outside a sandbox only through its own grants and policy. |

Example:

```sql
grant select on table app.customer to app_support;
grant select on column app.customer.customer_id to app_support;
grant select on column app.customer.display_name to app_support;
```

The table grant does not require every column to be visible. The final projection is the intersection of relation, column, row, mask, and protected-material policy.

## Sandbox Rules

Sandboxing applies before ordinary object visibility:

- a sandboxed session cannot grant, revoke, describe, or resolve objects outside its sandbox root unless an authorized administrative route admits it;
- granting privileges on a catalog projection does not grant privileges on the underlying hidden objects;
- a role active inside one sandbox does not automatically become active in another sandbox;
- bridge and external-access privileges are checked in addition to object privileges;
- names that spell parent paths outside the sandbox must fail closed.

## SHOW And DESCRIBE

Security inspection is itself security-sensitive.

```sql
show grants for app_reporter;
show privileges on table app.orders;
show roles;
describe role app_reader;
describe user app_reporter;
```

Inspection results should include only metadata the caller is authorized to see:

- principal UUID or redacted identifier;
- role membership and active/admin option state;
- target object class and name where visible;
- privilege name;
- grant option or admin option state;
- grantor where visible;
- security epoch;
- dependency or cascade readiness;
- refusal or redaction reason where policy allows disclosure.

## Transaction, Recovery, And Audit Behavior

Security DCL is catalog mutation and therefore follows MGA transaction finality.

| Event | Required Behavior |
| --- | --- |
| Commit | Grant, revoke, identity, role, and group changes become visible through a new security epoch. |
| Rollback | Changes disappear and prior effective privilege state remains visible. |
| Crash before commit | Recovery must expose old state or recovery-required fail-closed state. |
| Crash after commit | Recovery must expose the committed state or recovery-required fail-closed state. |
| Cache invalidation | Prepared statements, metadata caches, authorization snapshots, support projections, and active role state are refreshed or refused. |
| Audit | Security DCL should produce authorized audit evidence without leaking protected material. |

## Practical Examples

### Read-Only Application Role

```sql
create role app_reader;
grant usage on schema app to app_reader;
grant select on table app.orders to app_reader;
grant select on table app.order_items to app_reader;
grant app_reader to app_reporter;
```

### Writer Role With Narrow Mutation

```sql
create role app_order_writer;
grant usage on schema app to app_order_writer;
grant insert, update on table app.orders to app_order_writer;
grant update on column app.orders.order_state to app_order_writer;
grant app_order_writer to app_service;
```

### Routine Execution

```sql
create role app_routine_runner;
grant execute on procedure app.close_order to app_routine_runner;
grant execute on function app.tax_amount to app_routine_runner;
```

Routine execution still runs with the routine's declared security mode and policy. Granting `EXECUTE` does not grant direct access to every object the routine references.

### Administrative Separation

```sql
create role app_security_admin;
grant manage security on database appdb to app_security_admin;
grant app_security_admin to app_owner with admin option;
```

Administrative grants should be narrow and auditable. A security administrator can manage the admitted security surface but still remains subject to explicit deny, sandbox, protected-material, and recovery-required states.

## Failure Modes

| Failure | Required Behavior |
| --- | --- |
| Unknown or hidden principal | Return not-found or hidden-object diagnostic according to disclosure policy. |
| Unknown or hidden target | Return not-found or hidden-object diagnostic according to disclosure policy. |
| Grantor lacks grant authority | Refuse before catalog mutation. |
| Grantee lacks attach or sandbox eligibility | Refuse or record a dormant grant only where policy explicitly admits it. |
| Invalid privilege for target class | Refuse during bind/admission. |
| Revocation would orphan dependent grant | Refuse under `RESTRICT`; require explicit `CASCADE` where admitted. |
| Role cycle | Refuse role membership that creates a cycle unless policy defines a bounded acyclic expansion. |
| Explicit denial exists | Refuse even when an allow grant is present. |
| Recovery uncertainty | Fail closed and require operator or recovery action. |

## Verification Checklist

| Check | Required Outcome |
| --- | --- |
| Parse | Security statement shape is recognized by SBsql. |
| Bind | Principals, roles, groups, targets, privileges, options, and dependencies resolve to UUIDs and descriptors. |
| Authorize | Grantor or revoker has authority to mutate the requested security edge. |
| Admit | SBLR route and result shape are accepted by the engine verifier. |
| Apply | Grant/revoke changes alter effective authorization only after commit. |
| Deny precedence | Explicit denial or refusal policy wins over allow. |
| Sandbox | Names outside the sandbox root do not resolve through grants. |
| Inspect | `SHOW` and `DESCRIBE` redact metadata according to disclosure policy. |
| Invalidate | Security epoch changes invalidate dependent execution and metadata state. |
| Recover | Crash/restart never leaves a silently inconsistent grant graph. |
