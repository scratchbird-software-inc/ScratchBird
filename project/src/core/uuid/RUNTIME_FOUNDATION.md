# Core UUID Runtime Foundation

This package implements `RUNTIME-002`: UUIDv7 engine identity primitives and donor UUID compatibility values.

## Scope

The package owns:

- UUID parsing and canonical formatting;
- UUID variant and version inspection;
- donor compatibility policy checks for UUID versions 1 through 7;
- UUIDv7-only typed engine UUID construction for database, cluster, filespace, schema, object, row, page, transaction, session, and principal identity;
- UUIDv1 time/node compatibility-value generation;
- UUIDv2 DCE security compatibility-value generation;
- UUIDv3 namespace/name MD5 compatibility-value generation;
- UUIDv4 random compatibility-value generation;
- UUIDv5 namespace/name SHA-1 compatibility-value generation;
- UUIDv6 reordered-time compatibility-value generation;
- UUIDv7 Unix-time compatibility-value generation;
- UUID diagnostic records.

## Hard rule

Only UUIDv7 values may be used as ScratchBird engine identity.

UUIDv1 through UUIDv6 are never ScratchBird engine identity. They are compatibility values only. A donor/client UUIDv1 through UUIDv6 value must be stored as data, alias, migration evidence, or compatibility-overlay material and mapped to a UUIDv7 engine identity before it can identify any ScratchBird database, catalog object, row, page, transaction, principal, route, policy, filespace, schema, or other authority object.

## Authority rules

- Engine-owned identity is UUIDv7 only.
- UUIDv1 through UUIDv6 may be accepted, generated, rendered, or stored only as donor compatibility values, external data values, aliases, migration evidence, or client-visible UUID values.
- Names are never durable identity.
- UUID kind is explicit and must not be inferred from a user-facing name.
- Nil UUIDs are rejected for typed runtime identity.
- `session` UUIDs are intentionally not durable identity, but internal session identity is still UUIDv7 when represented as a typed engine UUID.
- UUIDv7 ordering is identity locality only. It is not commit order, visibility order, cleanup order, authorization authority, route authority, or cluster finality authority.
