# Schemas, Objects, And Names

## Purpose

In most databases, a name is how you find a thing. In ScratchBird, names are user-facing labels, but the engine stores durable identity separately — objects are tracked by UUID-backed catalog entries, not just by the text you type. That distinction matters the moment you rename an object, work through a compatibility parser, or write a migration script.

This page explains how to think about schemas, objects, qualified names, current schema, home schema, compatibility workareas, and recursive schema trees. For complete syntax, use the Language Reference. This page is the end-user orientation.

This page continues the tutorial arc from [First SBsql Session](first_sbsql_session.md), where you created a schema and a table. Now you will understand why those names worked the way they did.

## Names Are User-Facing Labels

A name is what a user types or sees in a tool:

```sql
select note_id, note_text
from app.notes;
```

In that example, `app` is a schema name, `notes` is an object name inside that schema, and `app.notes` is a qualified name. The engine does not rely only on text names for durable identity — objects are represented by catalog identity, descriptors, parent schema identity, grants, and transaction visibility.

## Durable Identity

ScratchBird uses UUID-backed catalog identity for durable database objects. Understanding this distinction prevents confusion when names change but references should not.

A visible name can change:

- an object can be renamed;
- an object can be displayed through a parser-specific catalog projection;
- a session can resolve an unqualified name through a current schema;
- a compatibility workarea (the namespace root presented to a compatibility client) can make one schema branch look like the client-visible root;
- a dependency can continue to point at the same object after a rename.

Durable identity lets the engine answer the deeper question: "Which object is this?" Names answer the user-facing question: "How does this session spell it?"

## Recursive Schema Tree

ScratchBird schemas can contain child schemas and database objects. This creates a schema tree rather than one flat namespace. A path through the tree gives context — `app.sales.orders` and `app.archive.orders` are different objects even though both end with `orders`.

![diagram](./schemas_objects_and_names-1.svg)

## Schema Context

A session can have several schema-related concepts at once. The relationship between them is what determines how an unqualified name like `notes` resolves to a specific object.

| Concept | Meaning |
| --- | --- |
| Database root | The top of the durable database tree. Not every session can see it directly. |
| Parser-visible root | The root of the namespace presented to the selected parser route. |
| Home schema | The schema associated with the connected identity or configured workarea. |
| Current schema | The default schema used for unqualified names in the current session. |
| Search path | An ordered list of schema locations used by commands that allow path-based lookup. |
| Object parent schema | The schema that owns an object's name within the tree. |

The exact variables and commands used to inspect these values are described in the Language Reference for the current release.

## Current Schema

The current schema is the default location for unqualified object names. Knowing your current schema is essential before running any statement that relies on unqualified names.

```sql
show schema path;

select note_id, note_text
from notes;
```

If the current schema is `app` and `notes` is visible in `app`, the unqualified name can resolve to `app.notes`. The command used to change the current schema is release-specific; consult the Language Reference for the current build.

Unqualified names are convenient in interactive sessions, but they can also hide mistakes. When writing administrative scripts, migrations, or examples intended for other users, prefer qualified names where clarity matters.

## Qualified Names

A qualified name includes path information, which makes it clear where the object is expected to live regardless of session schema state.

```sql
select note_id, note_text
from app.notes;
```

In a recursive schema tree, deeper paths may be used where supported:

```sql
select order_id, order_status
from app.sales.orders;
```

The binder still has to resolve the visible path to a real object identity. A qualified name is not a bypass around security, sandboxing, or transaction visibility.

## Name Resolution

Name resolution is the process of turning user-visible text into engine object identity. Understanding the steps helps you predict where a name lookup will fail and why.

At a high level, name resolution considers:

1. the parser route;
2. the authenticated identity;
3. the parser-visible schema root;
4. the current schema;
5. the search path where applicable;
6. explicit qualification in the statement;
7. grants, policy, and object visibility;
8. transaction visibility for recently created, changed, or dropped objects.

![diagram](./schemas_objects_and_names-2.svg)

If any step fails, the result should be a diagnostic rather than an accidental lookup outside the intended scope.

## Common Object Types

End users will commonly encounter these object categories. Not every parser route exposes every object category in the same way; native SBsql is the reference language for ScratchBird object administration.

| Object Type | What It Represents |
| --- | --- |
| Schema | A branch in the namespace tree. |
| Table | Stored rows and column descriptors. |
| Temporary table | A table whose data lifetime is scoped by session or transaction according to the table definition. |
| View | A named query projection. |
| Materialized view | A stored projection that must have refresh and dependency behavior defined by the implementation. |
| Index | A search structure over table or expression data. |
| Constraint | A rule attached to a table, column, or domain. |
| Domain | A reusable constrained type definition. |
| Type descriptor | A named type or type shape known to the engine. |
| Sequence | A database object that generates ordered values according to its definition. |
| Procedure | A stored routine that can perform controlled work. |
| Function | A stored or built-in routine that returns a value or result. |
| Package | A named grouping of routine definitions where supported. |
| Trigger | Routine behavior tied to table, database, transaction, or event-style actions where implemented. |
| Policy | A named rule for row access, masking, external access, or operational admission. |
| Role and privilege | Security objects that describe who can do what. |
| Comment | User-facing descriptive metadata attached to an object. |

## Object Lifecycle

Most schema objects follow a lifecycle from creation through use to eventual removal. Some object types also support `recreate`, `create or alter`, refresh, attach, detach, validate, or other object-specific actions. The object page in the Language Reference is the authoritative place for supported lifecycle syntax.

![diagram](./schemas_objects_and_names-3.svg)

## Comments And Descriptions

Comments are descriptive metadata. They help tools and users understand objects, but they do not grant access and do not change object identity.

Use comments for purpose, ownership notes, migration context, operational warnings, column meaning, and expected units for values. Avoid putting secrets, passwords, tokens, or protected operational details in comments.

## Compatibility Workareas

A compatibility parser session normally sees a workarea as its root. That means a client can experience the connected workarea as "the database" even though ScratchBird may store it as a branch inside a larger recursive schema tree.

![diagram](./schemas_objects_and_names-4.svg)

The client cannot simply name `Outside` and access it. If a catalog projection shows selected metadata, that projection is an object with its own authority — it is not proof that the connected user can directly query the underlying object.

## Case, Quoting, And Identifiers

Identifier behavior can vary by parser route. Native SBsql is intended to remain context-sensitive with as few reserved words as practical, but scripts should still be written carefully:

- use clear object names;
- avoid names that differ only by case;
- avoid names that look like built-in functions or command keywords;
- use qualified names in administrative scripts;
- quote identifiers only when the language reference says quoting is required or intended;
- keep migration scripts consistent about naming style.

## Object Defaults Can Be Parser-Specific

Two parser routes can expose similar object concepts while applying different defaults. Examples include index null behavior, identifier folding, default schema selection, default datatype precision, string literal handling, generated name formatting, catalog projection rows, and diagnostic wording. The engine still owns the durable object; the parser is responsible for mapping its client-facing defaults into an explicit engine request.

## Practical Naming Guidance

For new ScratchBird-native work:

- create an application schema instead of placing user objects at the root;
- qualify object names in migration and administration scripts;
- choose stable names that describe the object's purpose;
- use comments for human-facing meaning;
- avoid secrets in object names and comments;
- avoid relying on implicit search paths in automated scripts;
- test rename behavior before using it in migration tooling;
- verify grants after moving or renaming important objects.

## Example Schema Layout

A small application might start with this shape:

```text
database_root
|-- app
|   |-- notes
|   |-- note_tags
|   |-- active_notes
|   `-- routines
|-- audit
|   `-- note_events
`-- policy
    `-- application_policies
```

That tree separates application data, audit records, and policy-related objects. The exact layout for a real application depends on authorization, operational needs, and migration strategy.

## Where To Go Next

This completes the core tutorial arc: you have created a database, worked through a session, and now understand how names and objects relate to each other. The next topic in depth is how compatibility parsers work and what it means for a ScratchBird database to expose a reference-system surface — read [Reference-System Compatibility](reference_system_compatibility.md) for that.

- [First SBsql Session](first_sbsql_session.md)
- [Reference-System Compatibility](reference_system_compatibility.md)
- [Recursive Schema Tree](../architecture/recursive_schema_tree.md)
- [Schema Tree And Name Resolution](../../Language_Reference/syntax_reference/schema_tree_and_name_resolution.md)
- [Schema Statements](../../Language_Reference/syntax_reference/schema.md)
- [Table Statements](../../Language_Reference/syntax_reference/table.md)
- [Script Tokens And Identifiers](../../Language_Reference/syntax_reference/script_tokens_and_identifiers.md)
