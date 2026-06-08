# Schemas, Objects, And Names

## Purpose

This page explains how end users should think about names in ScratchBird.

## Names Are Labels

ScratchBird lets users work with names such as `app.notes`. The engine stores durable identity separately. Object UUIDs, parent schema UUIDs, descriptors, grants, and transaction visibility are what make an object authoritative.

## Common Object Types

End users will most often encounter:

- schemas;
- tables;
- views;
- indexes;
- constraints;
- domains and types;
- procedures and functions;
- triggers;
- sequences;
- comments;
- privileges.

Different parser profiles may expose different spellings and defaults for these objects.

## Current Schema

The current schema is the default schema for unqualified names. If the current schema is `app`, then `select * from notes` can resolve to `app.notes` if that object exists and is visible.

## Qualified Names

A qualified name gives more path information:

```sql
select *
from app.notes;
```

Qualified names are still labels. The binder must resolve them to visible object UUIDs.

## Donor Workareas

A donor parser session normally treats its connected donor workarea as the root. That means the same underlying ScratchBird database can present a donor-shaped view to a donor client and a broader administrative view to an authorized SBsql session.

## Related Pages

- [../architecture/recursive_schema_tree.md](../architecture/recursive_schema_tree.md)
- [../../Language_Reference/syntax_reference/schema_tree_and_name_resolution.md](../../Language_Reference/syntax_reference/schema_tree_and_name_resolution.md)
