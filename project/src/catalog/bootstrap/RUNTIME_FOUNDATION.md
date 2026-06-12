# Catalog Bootstrap Runtime Foundation

This package implements `RUNTIME-017`: database UUID bootstrap, root schema UUID bootstrap, `sys.catalog`, `sys.metrics`, and local user schema roots.

## Scope

The package owns:

- database bootstrap identity;
- root schema bootstrap records;
- the initial local recursive schema tree roots;
- system schema authority flags;
- deterministic diagnostics for invalid bootstrap manifests.

## Identity rules

- Database UUIDs are engine identity and must be UUIDv7.
- Schema root UUIDs are engine identity and must be UUIDv7.
- UUIDv1 through UUIDv6 are reference/client compatibility values only and cannot be catalog bootstrap identity.
- Names and paths are labels over UUID identity, not durable authority.

## Bootstrap roots

The first local bootstrap manifest requires:

- `sys.catalog`, engine-owned and not user mutable;
- `sys.metrics`, engine-owned and not user mutable;
- `local.user`, user mutable and not engine-owned.

The recursive schema tree is mandatory. A bootstrap manifest that disables it fails closed.

## Non-scope

This slice does not implement persistent catalog pages, catalog object rows, localized-name tables, comments, grants, policies, or cluster schema roots. Those are later slices.

## RUNTIME-018 catalog identity and localized labels

This package also implements `RUNTIME-018`: catalog row UUID versus object UUID separation, localized names, localized comments, and UUID lookup placeholders.

The catalog identity layer owns:

- catalog rows as row-identified records;
- SQL objects as separate object-identified entities;
- explicit object kinds;
- localized names and paths;
- localized comments;
- UUID-first lookup placeholders.

## Row UUID versus object UUID rules

- A catalog row is a row and therefore has a row UUID.
- The SQL object described by a catalog row has an object UUID.
- The catalog row UUID and object UUID must be different.
- Both UUIDs are engine identity and must be UUIDv7.
- UUIDv1 through UUIDv6 are reference/client compatibility values only and cannot be catalog row identity or SQL object identity.

## Name and comment rules

- Names and paths are localized labels over object UUIDs.
- Names and paths are not durable identity.
- Name resolution returns object UUIDs before engine-owned work begins.
- A localized path that resolves to different visible object UUIDs is an ambiguity error.
- Comments attach to object UUIDs, not names.

## Lookup placeholder rules

- Lookup by object UUID is authoritative for this slice.
- Lookup by localized path is a parser/catalog convenience that resolves to object UUID.
- This slice does not implement security filtering, recursive tree traversal, persistent catalog storage, or cluster-shared catalog roots.

## DBOPEN-003 catalog persistence image

This package now includes the first persistable catalog bootstrap image for the database create/open vertical slice.

The catalog persistence image owns:

- the local bootstrap manifest;
- catalog object identities for bootstrap database/schema objects;
- catalog row UUID versus object UUID separation;
- localized default names over object UUIDs;
- localized bootstrap comments over object UUIDs;
- explicit marker that final catalog page body layout is not yet available.

The image is a deterministic bootstrap contract. It is not the final catalog table/page storage format.
