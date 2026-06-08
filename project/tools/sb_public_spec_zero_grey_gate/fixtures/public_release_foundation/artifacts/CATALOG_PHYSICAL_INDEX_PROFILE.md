# System Catalog Physical Index Profile

Search key: `PUBLIC_RELEASE_FOUNDATION_CATALOG_INDEX_PROFILE`

## Authority Boundary

`sys.catalog` stores low-level system authority. It may be human-readable, but
it must not store human-interface names as object authority. Object names,
aliases, localized names, and comments live in resolver/projection tables.

## Index Families

| Access Path | Required Index Profile |
| --- | --- |
| Object UUID equality | Hash index on object UUID. |
| Row UUID equality | Hash index on row UUID. |
| Parent-child catalog traversal | Btree on parent UUID plus object class plus lifecycle state, with UUID hash for point lookup. |
| Name resolution | Btree on parent UUID plus normalized name plus language tag plus name class, with hash on target object UUID. |
| Reverse name lookup | Hash on object UUID plus btree on language tag/name class for preferred rendering. |
| Dependency outgoing edges | Btree on source object UUID plus dependency class plus target object UUID. |
| Dependency incoming edges | Btree on target object UUID plus dependency class plus source object UUID. |
| Catalog generation scans | Btree on catalog generation ID plus transaction UUID. |
| Transaction visibility scans | Btree on creator transaction plus retired transaction plus lifecycle state. |
| Diagnostic lookup | Hash on diagnostic code and btree on severity/category for reporting. |

## UUID Btree Use

UUID values are not generally useful for human range selection. Btree indexes on
UUID fields are allowed only when the ordered key is part of a compound access
path needed for traversal, generation ordering, dependency scans, or physical
verification. UUID point lookup uses hash indexes unless the storage subsystem
requires a btree backing structure for uniqueness enforcement.

## Acceptance

The `catalog_physical_index_profile_gate` fails if catalog table
materialization creates a required access path without the index profile above,
or if a catalog authority table duplicates human-readable object names outside
resolver/projection tables.
