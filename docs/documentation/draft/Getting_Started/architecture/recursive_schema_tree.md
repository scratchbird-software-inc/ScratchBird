# Recursive Schema Tree

## Purpose

After reading this page you will understand why ScratchBird organizes schemas as a nested tree rather than a flat list, and why that choice matters for security, compatibility, and database organization.

In many database systems, all schemas share a single level — every schema is directly visible to every connected user (subject to grants). ScratchBird takes a different approach: schemas form a tree, so an application can live under its own branch, a compatibility parser can see only its assigned workarea (a schema branch presented as the client's visible database root), and system objects are separated from user objects by design. The recursive schema tree is central to native SBsql administration, compatibility workareas, sandboxing, catalog projections, and durable UUID-backed object identity.

## Basic Shape

The names below are explanatory labels, not a required database layout.

```text
database_root
|-- system
|   |-- catalog
|   |-- security
|   |-- diagnostics
|   `-- storage
|-- users
|   |-- public
|   `-- home
|-- applications
|   |-- app
|   |   |-- tables
|   |   |-- routines
|   |   `-- policy
|   `-- audit
`-- workareas
    |-- compatibility_area_a
    `-- compatibility_area_b
```

Engine identity is UUID-based. The visible names are labels resolved by the session.

## Why Recursive Schemas Exist

Recursive schemas let ScratchBird represent several ideas without flattening them into one global namespace.

| Need | How The Tree Helps |
| --- | --- |
| Application organization | Application objects can live under a branch. |
| Administrative separation | System, security, diagnostics, and storage metadata can be separated from user objects. |
| User home areas | A user's default namespace can be a branch rather than a single global schema. |
| Compatibility workareas | A parser can present one branch as the client's database root. |
| Catalog projections | Metadata views can be placed where the intended users can see them. |
| Policy scoping | Policy can be attached or reasoned about by branch. |
| Migration staging | Imported or converted objects can be staged in a separate branch before promotion. |

## Durable Identity Versus Path Names

Path names like `applications.app.tables.notes` are convenient for humans, but they are not what SBcore uses as the authoritative record. Every durable object is identified by a UUID, and the path name is just a label that resolves to that identity during name binding. This separation is what makes renames safe: the object's identity does not change, only its visible label does.

A path such as `applications.app.tables.notes` is user-facing. The durable object is represented by catalog identity and descriptors (the engine's metadata records for objects, types, and constraints).

That distinction matters because:

- an object can be renamed;
- a branch can be moved or reorganized where supported;
- parser routes can render names differently;
- dependencies should follow object identity;
- grants should apply to the intended object;
- transaction visibility controls whether a catalog change is visible.

Names are necessary for users. UUID identity is necessary for durable engine authority.

## Session Views

One of the most practical consequences of the tree model is that different sessions can be shown different portions of it. An administrative user may navigate the full tree. An application user may only see the branch their application lives under. A compatibility parser session may see its workarea as if it were the entire database. This is part of the security model, not a display feature — the visible root limits which objects a session can name and access.

Different sessions can see different roots.

![diagram](./recursive_schema_tree-1.svg)

An authorized administrative SBsql session may see broad portions of the tree. A normal application user may see an application branch. A compatibility parser session may see its workarea as the root.

The visible root is part of the security model, not just a display preference.

## Schema Context Variables

ScratchBird documentation uses several schema-context ideas.

| Concept | Meaning |
| --- | --- |
| Database root | The top of the durable database tree. |
| Parser-visible root | The root presented to the selected parser route. |
| Home schema | The schema associated with a user, service, or configured workarea. |
| Current schema | The default schema for unqualified names in the current session. |
| Search path | An ordered set of schemas used by commands that allow path-based lookup. |
| Object parent schema | The schema that owns a specific object's local name. |

The exact inspection and assignment syntax belongs in the Language Reference.

## Name Resolution

Name resolution turns text into object identity.

![diagram](./recursive_schema_tree-2.svg)

An unqualified name such as `notes` may resolve through the current schema. A qualified name such as `app.notes` gives more path information. Neither form bypasses visibility, grants, policy, or transaction state.

## Compatibility Workareas

A compatibility workarea is a schema branch presented to a parser route as the client-visible database root. The parser route sees a familiar namespace; the rest of the ScratchBird tree remains outside the client's visible scope. This is how ScratchBird can host multiple parser routes — each with its own default namespace expectations — on the same underlying database without those routes interfering with each other.

This lets the parser show a client a familiar namespace without giving that client direct access to the entire ScratchBird tree.

```text
database_root
|-- workareas
|   `-- accounting_compat
|       |-- catalog_projection
|       |-- tables
|       |-- views
|       `-- routines
`-- internal
    `-- not_visible_to_that_client
```

A catalog projection can expose selected metadata if the projection object has authority. That is different from giving the connected user direct access outside the workarea.

## Current Schema Examples

An SBsql session resolves unqualified names through the current schema path. The command that changes the current schema is release-specific; use `show schema path;` to inspect the active path.

```sql
create schema app;
show schema path;

create table app.notes (
    note_id bigint not null,
    note_text text not null
);

select note_id, note_text
from notes
order by note_id;
```

The unqualified name `notes` resolves relative to the current schema path. A more explicit script can use qualified names:

```sql
select note_id, note_text
from app.notes
order by note_id;
```

Use qualified names for administrative scripts and migrations when ambiguity would be expensive.

## Object Lifecycle In The Tree

Object lifecycle operations interact with the tree.

| Operation | Tree Effect |
| --- | --- |
| Create schema | Adds a branch under a parent schema. |
| Create object | Adds an object under a parent schema. |
| Rename object | Changes a visible label while preserving durable identity where supported. |
| Move object | Changes parent context where supported and authorized. |
| Comment on object | Adds descriptive metadata without changing authority. |
| Drop object | Removes or marks the object according to transaction visibility and dependency rules. |
| Describe or show | Presents visible metadata through the current parser route. |

Dependency and authorization checks should prevent unsafe changes.

## Practical Guidance

For new SBsql work:

- create application schemas deliberately;
- avoid placing application objects at the database root;
- qualify names in migrations;
- avoid names that differ only by case;
- do not rely on catalog projections as direct access authority;
- document the intended schema root for each application or parser route;
- verify name resolution after renames;
- include explicit `order by` in result checks when order matters.

## Where To Go Next

- [Schemas, Objects, And Names](../using_scratchbird/schemas_objects_and_names.md)
- [Identity, Authentication, And Authorization](identity_authentication_and_authorization.md)
- [Engine Parser Boundary](engine_parser_boundary.md)
- [Schema Tree And Name Resolution](../../Language_Reference/syntax_reference/schema_tree_and_name_resolution.md)
- [Schema Statements](../../Language_Reference/syntax_reference/schema.md)
