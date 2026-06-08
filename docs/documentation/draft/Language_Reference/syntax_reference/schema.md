# Schema Lifecycle

This page is part of the SBsql Language Reference Manual. It documents schema creation, alteration, rename, recreation, comments, inspection, and drop behavior. The deeper resolver model, session schema variables, recursive branch layout, search path, base schema tree, and sandbox rules are documented in [Schema Tree And Name Resolution](schema_tree_and_name_resolution.md).

Generation task: `syntax_reference_schema_lifecycle`

Related pages: [Schema Tree And Name Resolution](schema_tree_and_name_resolution.md), [Database Lifecycle](database.md), [Table Lifecycle](table.md), [View Lifecycle](view.md), [Domain Lifecycle](domain.md), [Security And Privileges](security_and_privilege_statements.md), [Policy, Mask, And RLS Lifecycle](policy_mask_and_rls.md), [Refusal Vectors](refusal_vectors.md), and [UUID Catalog Identity](../core_paradigms/uuid_catalog_identity.md).

## Purpose

A schema is both a catalog object and a resolver branch. It owns a durable schema UUID, a parent schema UUID unless it is a root branch, localized names, comments, grants, policies, default descriptors, and child-object namespaces. SQL text names such as `app` or `users.alice.app` are resolver input; they are not durable identity.

Schema lifecycle statements mutate the schema tree. They do not mutate table rows directly, but they affect how names bind, how unqualified object creation chooses a parent scope, what metadata a session can inspect, and which dependent plans or metadata caches remain valid.

## Complete Lifecycle Surface

| Operation | Surface | Contract |
| --- | --- | --- |
| Create | `CREATE SCHEMA` | Creates a durable schema UUID and name-registry entry under an admitted parent schema. |
| Alter | `ALTER SCHEMA` | Changes admitted schema metadata without changing the schema UUID. |
| Rename | `RENAME SCHEMA ... TO ...` | Changes resolver labels while preserving schema UUID, child object UUIDs, grants, dependencies, and policy links. |
| Recreate | `RECREATE SCHEMA` | Drops and recreates a schema branch as one statement only when dependency, privilege, sandbox, and recovery rules admit the replacement. |
| Comment | `COMMENT ON SCHEMA ... IS ...` | Stores or clears descriptive metadata through the schema catalog row. |
| Show | `SHOW SCHEMA`, `SHOW SCHEMAS` | Lists authorized schema branches and selected properties. Hidden branches are omitted. |
| Describe | `DESCRIBE SCHEMA` | Returns authorized metadata for one schema, including child object classes, defaults, owner, policy, and dependency summary. |
| Drop | `DROP SCHEMA` | Retires or removes a schema branch after dependency handling, privilege checks, and transaction admission. |

Schema statements are transactional catalog mutations. A created, altered, renamed, recreated, or dropped schema becomes visible only when the owning transaction reaches MGA finality.

## Syntax

```ebnf
schema_lifecycle_statement ::=
      create_schema_statement
    | alter_schema_statement
    | rename_schema_statement
    | recreate_schema_statement
    | comment_on_schema_statement
    | show_schema_statement
    | describe_schema_statement
    | drop_schema_statement ;
```

```ebnf
create_schema_statement ::=
    CREATE SCHEMA if_not_exists? schema_ref schema_create_clause* ;

if_not_exists ::= IF NOT EXISTS ;

schema_create_clause ::=
      AUTHORIZATION principal_ref
    | UNDER schema_ref
    | WITH schema_option_list ;
```

```ebnf
alter_schema_statement ::=
    ALTER SCHEMA schema_ref alter_schema_action+ ;

alter_schema_action ::=
      SET OWNER principal_ref
    | SET DEFAULT CHARACTER SET identifier
    | SET DEFAULT COLLATION qualified_name
    | SET DEFAULT FILESPACE filespace_ref
    | SET DEFAULT POLICY policy_ref
    | SET VISIBILITY schema_visibility
    | SET COMMENT string_literal
    | RESET DEFAULT CHARACTER SET
    | RESET DEFAULT COLLATION
    | RESET DEFAULT FILESPACE
    | RESET DEFAULT POLICY
    | RESET VISIBILITY ;
```

```ebnf
rename_schema_statement ::=
    RENAME SCHEMA schema_ref TO identifier ;

recreate_schema_statement ::=
    RECREATE SCHEMA schema_ref schema_create_clause* ;

comment_on_schema_statement ::=
    COMMENT ON SCHEMA schema_ref IS (string_literal | NULL) ;
```

```ebnf
show_schema_statement ::=
      SHOW SCHEMAS show_schema_filter?
    | SHOW SCHEMA schema_ref show_schema_option_list? ;

show_schema_filter ::=
      LIKE string_literal
    | WHERE predicate ;

show_schema_option_list ::=
    WITH show_schema_option ("," show_schema_option)* ;

show_schema_option ::=
      CHILDREN
    | DEFAULTS
    | POLICIES
    | GRANTS
    | DEPENDENCIES
    | UUIDS ;
```

```ebnf
describe_schema_statement ::=
    DESCRIBE SCHEMA schema_ref describe_schema_option_list? ;

describe_schema_option_list ::=
    WITH describe_schema_option ("," describe_schema_option)* ;

describe_schema_option ::=
      CHILDREN
    | OBJECTS
    | DEFAULTS
    | POLICIES
    | GRANTS
    | DEPENDENCIES
    | UUIDS ;
```

```ebnf
drop_schema_statement ::=
    DROP SCHEMA if_exists? schema_ref drop_schema_behavior? ;

if_exists ::= IF EXISTS ;

drop_schema_behavior ::=
      RESTRICT
    | CASCADE ;
```

SBsql is context sensitive. Words such as `schema`, `authorization`, `under`, `visibility`, `children`, and `dependencies` are command words in the schema lifecycle context; they do not need to be globally reserved identifiers in unrelated expression positions.

## Schema References

A schema reference resolves either by UUID or by qualified name:

```ebnf
schema_ref ::= uuid_ref | qualified_name ;
```

Qualified schema names are recursive paths through the schema tree:

```sql
create schema app;
create schema app.archive;
create schema users.alice.workspace;
```

The text path is a set of resolver labels. The durable identity is the resolved schema UUID. Renaming `app.archive` changes the path label, not the UUID of the schema or its child objects.

## Create Schema

`CREATE SCHEMA` creates a schema branch under an admitted parent. If the name is unqualified, the parent is the current schema or the statement's explicit target schema, according to the active session descriptor. If the name is qualified, each parent path component must resolve, be visible, and admit child-schema creation.

Basic creation:

```sql
create schema app;
```

Creation under a recursive parent:

```sql
create schema app.reporting;
```

Creation with explicit parent:

```sql
create schema reporting under app;
```

Creation with owner and defaults:

```sql
create schema app_ingest
authorization app_owner
with
  default character set utf8,
  default collation sys.fn.unicode_ci,
  default filespace primary_data;
```

The binder must prove:

- the effective principal has `CREATE SCHEMA` on the database or parent schema;
- the parent schema is visible inside the session sandbox root;
- no visible sibling with the same normalized label already exists;
- the owner principal resolves and may own the schema;
- default charset, collation, filespace, and policy descriptors exist and are admitted;
- the catalog mutation route is available and accepted by SBLR admission;
- the database is not fenced against catalog writes.

`IF NOT EXISTS` suppresses the duplicate-name error only when the existing visible object is a schema compatible with the requested shape. It must not hide a privilege failure, sandbox escape, incompatible object class, descriptor mismatch, or recovery fence.

## Schema Options

| Option | Meaning | Refusal Conditions |
| --- | --- | --- |
| `AUTHORIZATION principal` | Sets the initial owner UUID. | Principal hidden, not admitted as owner, or caller lacks owner assignment authority. |
| `UNDER schema` | Sets the parent schema explicitly. | Parent not found, hidden, outside sandbox, or caller lacks child-create privilege. |
| `DEFAULT CHARACTER SET` | Default character set for child objects that inherit it. | Descriptor unknown, unsupported, or not admitted by policy. |
| `DEFAULT COLLATION` | Default collation descriptor for child textual objects. | Collation incompatible with default charset or not admitted. |
| `DEFAULT FILESPACE` | Default filespace for child objects that do not specify one. | Filespace hidden, read-only, incompatible, or outside policy. |
| `DEFAULT POLICY` | Default policy applied to admitted child objects. | Policy not visible, wrong class, or not attachable to schemas. |
| `VISIBILITY` | Metadata visibility profile for the branch. | Caller lacks policy/metadata administration authority. |
| `COMMENT` | Initial descriptive metadata. | Comment policy rejects size, language, classification, or protected material. |

Unsupported option combinations return a refusal vector before any catalog mutation is created.

## Alter Schema

`ALTER SCHEMA` changes metadata attached to the existing schema UUID. It must not rewrite child object identity.

Examples:

```sql
alter schema app
  set owner app_owner;

alter schema app
  set default filespace app_data
  set default policy app_table_policy;

alter schema app
  reset default collation;
```

Alteration requires schema `ALTER` privilege plus any authority required by the specific action. For example, changing the owner requires owner-assignment authority, changing defaults requires access to the target descriptor, and changing policy metadata requires policy administration authority.

Altering a default affects future child object creation. Existing child objects keep their own descriptors unless an explicit child-object alteration changes them.

## Rename Schema

`RENAME SCHEMA` changes the resolver label of a schema under its current parent.

```sql
rename schema app.reporting to reports;
```

After the rename, `app.reports` resolves to the same schema UUID that `app.reporting` resolved to before commit. Dependencies stored by UUID remain valid. Textual metadata, support projections, prepared statements, and client metadata caches that rely on the old path must be invalidated.

A rename is refused when:

- the new label conflicts with a visible sibling;
- the target schema is a protected system branch;
- the caller lacks `ALTER` or rename authority;
- the branch is outside the session sandbox root;
- a policy freezes the branch name;
- the database is in a recovery or read-only state that fences catalog writes.

## Recreate Schema

`RECREATE SCHEMA` is a controlled replacement surface. It is not a shortcut around dependency checks.

```sql
recreate schema app_stage;
```

The operation is admitted only when the effective behavior is unambiguous:

- if the schema does not exist, create it;
- if it exists and is empty or otherwise admitted for replacement, retire the old branch and create the new branch transactionally;
- if it contains child objects or dependent grants/policies, require explicit dependency handling or refuse.

`RECREATE SCHEMA` should be used for deployment scripts that need deterministic replacement of an admitted empty or staging branch. It must not silently cascade through production objects.

## Comment On Schema

`COMMENT ON SCHEMA` stores descriptive metadata on the schema catalog row.

```sql
comment on schema app is 'Application-owned objects';
comment on schema app is null;
```

Comments are metadata, not authority. A comment may be hidden, redacted, localized, versioned, or omitted from inspection results according to disclosure policy. Setting a comment requires `COMMENT` privilege or an admitted ownership/admin authority.

## Show And Describe

`SHOW SCHEMAS` lists authorized branches.

```sql
show schemas;
show schemas like 'app%';
```

`SHOW SCHEMA` displays selected properties for one schema:

```sql
show schema app with defaults, policies, uuids;
```

`DESCRIBE SCHEMA` returns a deeper object-oriented view:

```sql
describe schema app with children, objects, dependencies;
```

Inspection is subject to metadata disclosure policy. If a schema is outside the sandbox root or hidden by authorization policy, it must be omitted or rendered as not visible according to [Refusal Vectors](refusal_vectors.md). `WITH UUIDS` returns durable identifiers only when the session is allowed to inspect UUIDs.

## Drop Schema

`DROP SCHEMA` removes or retires a schema branch after dependency handling.

```sql
drop schema app_stage restrict;
```

`RESTRICT` refuses the drop when the schema contains child schemas, tables, views, routines, policies, grants, comments, external descriptors, temporary-object dependencies, or active metadata references that must be handled first.

`CASCADE` is explicit dependency handling. It still requires authorization for every affected child object and must produce an auditable dependency plan before mutation. If any affected object cannot be dropped, the entire statement is refused or rolled back according to transaction rules.

`IF EXISTS` suppresses "not found" for an absent visible schema. It does not suppress privilege failures, hidden-object refusal, sandbox denial, recovery fences, or dependency errors.

Protected system branches and bootstrap roots cannot be dropped through ordinary schema DDL.

## Recursive Branch Rules

Schema lifecycle statements must preserve the recursive schema-tree contract:

| Rule | Required Behavior |
| --- | --- |
| Parent UUID | Every non-root schema records a parent schema UUID. |
| Sibling uniqueness | Two visible child schemas under the same parent cannot share the same normalized label. |
| Durable identity | Rename and alter preserve the schema UUID. |
| Child identity | Child object UUIDs remain stable when a parent schema is renamed. |
| Sandbox boundary | A session cannot create, inspect, rename, or drop outside its admitted root. |
| Transaction visibility | Other transactions see the change only through MGA visibility rules. |
| Cache invalidation | Resolver caches, prepared statements, metadata views, and support projections are invalidated when schema path metadata changes. |
| Disclosure policy | Hidden parent or child branches are not revealed by diagnostics unless policy admits disclosure. |

## Privileges

Schema operations require object and context privileges.

| Operation | Typical Required Authority |
| --- | --- |
| `CREATE SCHEMA` | `CREATE SCHEMA` on the database or parent schema. |
| `ALTER SCHEMA` | `ALTER` on the schema plus authority for changed descriptors. |
| `RENAME SCHEMA` | `ALTER` or rename authority on the schema and parent branch. |
| `RECREATE SCHEMA` | Create authority plus drop/replace authority when the old branch exists. |
| `COMMENT ON SCHEMA` | `COMMENT` on the schema or admitted ownership authority. |
| `SHOW SCHEMAS` | Metadata visibility on listed schemas. |
| `DESCRIBE SCHEMA` | `DESCRIBE` on the schema and disclosure rights for requested fields. |
| `DROP SCHEMA` | `DROP` on the schema and required authority over affected child objects. |

Ownership is not a parser bypass. The engine verifies effective user or agent UUID, active roles, group membership, explicit denies, object grants, inherited policy, sandbox root, and recovery state before catalog mutation.

## Transaction And Recovery Semantics

Schema DDL participates in the active transaction unless the surrounding execution context explicitly admits a different transactional mode.

- A schema created and then rolled back must not remain visible.
- A rename rolled back must restore the prior resolver label.
- A dropped schema rolled back must preserve the prior branch and child-object bindings.
- A committed schema mutation must invalidate stale resolver and plan evidence.
- A recovery-required database fences schema writes until recovery policy admits them.
- Prepared statements that bind through old schema evidence must rebind or fail closed.

MGA is the final transaction authority. Parser output, SQL text, client metadata, and support projections are evidence only.

## Examples

Create an application branch and child schemas:

```sql
create schema app;
create schema app.current;
create schema app.archive;

comment on schema app is 'Application root';
describe schema app with children, defaults;
```

Set defaults for future objects:

```sql
alter schema app
  set default character set utf8
  set default collation sys.fn.unicode_ci
  set default filespace app_data;
```

Rename a child branch without changing object identity:

```sql
rename schema app.archive to historical;

show schema app.historical with uuids;
```

Drop a staging branch only if it is empty:

```sql
drop schema app_stage restrict;
```

Inspect only authorized schemas:

```sql
show schemas where name starts with 'app';
```

## Refusal Conditions

Schema statements return refusal vectors for recognized but inadmissible requests.

| Condition | Refusal Class |
| --- | --- |
| Unknown or unsupported option | `unsupported` |
| Schema outside sandbox root | `denied` |
| Missing create, alter, comment, describe, or drop privilege | `denied` |
| Hidden parent or target branch | `denied` or not-visible rendering according to policy |
| Duplicate visible sibling name | bind diagnostic or `denied` when disclosure is restricted |
| Protected system branch mutation | `denied` |
| Descriptor default not admitted | `denied` or `unsupported` according to reason |
| Dependency plan incomplete | `denied` |
| Recovery-required catalog fence | `denied` |
| Product profile omits a gated schema operation | `unlicensed` or `unsupported` according to route admission |

## Verification Checklist

| Check | Required Outcome |
| --- | --- |
| Parse | Every lifecycle statement shape is recognized as SBsql schema DDL or inspection. |
| Bind | Schema references resolve to UUIDs or produce stable diagnostics. |
| Create | New schema rows carry parent UUID, owner UUID, name-registry entry, defaults, and catalog generation. |
| Alter | Metadata changes preserve schema UUID and child object UUIDs. |
| Rename | Old path invalidates, new path binds to the same schema UUID after commit. |
| Recreate | Replacement is atomic and refuses unsafe dependency cases. |
| Comment | Comments persist, clear with `NULL`, and obey disclosure policy. |
| Show/describe | Inspection omits hidden branches and redacts protected fields. |
| Drop | `RESTRICT` refuses dependencies; `CASCADE` requires explicit authorized dependency handling. |
| Transaction | Rollback removes uncommitted schema changes; commit publishes them through MGA finality. |
| Recovery | Recovery fences refuse catalog writes until admitted. |
| Proof | Full rebuild tests regenerate parser, SBLR, catalog, security, transaction, and refusal evidence. |
