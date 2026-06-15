# Metadata: sys_information_recursive

## Purpose

This page describes the `sys_information_recursive` metadata profile — the profile used by
all ScratchBird drivers and adaptors. It explains how drivers introspect database schemas,
tables, columns, indexes, and other catalog objects via the `sys.information.*` recursive
catalog projections.

Sources used: `project/drivers/DriverPackageManifest.csv`,
`project/drivers/driver/python/S3_METADATA_IMPLEMENTATION.md`.

This is a **draft**. Components are in `beta_2` / `release_candidate` status.

---

## Profile Identification

| Profile Name | Manifest Value | Used by |
| --- | --- | --- |
| sys.information recursive | `sys_information_recursive` | All 21 drivers, all 12 adaptors, and the CLI tool |

Source: `DriverPackageManifest.csv` column `metadata_profile`.

---

## What the Profile Provides

The `sys_information_recursive` profile exposes catalog metadata through the
`sys.information.*` namespace of recursive catalog projections. Unlike flat information
schemas, these projections support recursive schema tree navigation — a schema can be a
parent of other schemas, and the projections let drivers traverse that tree.

Drivers expose metadata through named **metadata collections**. The following collections
are supported:

| Collection | Contents |
| --- | --- |
| `schemas` | Database schemas (supports recursive parent-expansion) |
| `tables` | Tables and views |
| `columns` | Columns within tables/views |
| `indexes` | Indexes |
| `index_columns` | Columns within indexes |
| `constraints` | Table constraints |
| `catalogs` | Database catalog entries |
| `primary_keys` | Primary key constraints |
| `foreign_keys` | Foreign key constraints |
| `procedures` | Stored procedures |
| `functions` | User-defined functions |
| `routines` | All routines (procedures + functions) |
| `table_privileges` | Table-level privilege grants |
| `column_privileges` | Column-level privilege grants |
| `type_info` | Type information |

Source: `project/drivers/driver/python/S3_METADATA_IMPLEMENTATION.md` — "first-class executable
metadata APIs on `Connection`" and "convenience metadata wrappers."

---

## Accessing Metadata

Drivers expose the following entry points for metadata access:

| Method | Purpose |
| --- | --- |
| `query_metadata(collection_name, restrictions)` | Execute a metadata query and return a cursor. Accepts a collection name (e.g., `'tables'`) and optional restriction filters. |
| `get_schema(collection_name, restrictions)` | Materialize metadata rows by draining cursor results. |
| `ddl_editor_schema_payload(schema_pattern, expand_schema_parents)` | Return a deterministic schema-navigation payload for DDL-editor consumers, including `schemaPaths` and a recursive `schemaTree`. |

Convenience wrappers are available for each collection (e.g., `.schemas()`, `.tables()`,
`.columns()`, `.primary_keys()`, etc.). Unsupported collections raise
`errors.NotSupportedError`.

Source: `project/drivers/driver/python/S3_METADATA_IMPLEMENTATION.md`.

---

## Restriction Filtering

Metadata queries accept restriction filters to narrow results. The filter system supports:

| Feature | Behavior |
| --- | --- |
| Wildcard matching | `%` (any sequence of characters) and `_` (any single character), with escape character support |
| Null matching | The string `"null"` matches NULL values |
| Alias-aware keys | Restriction keys are normalized through alias mappings |
| Unknown key handling | Unrecognized restriction keys are ignored rather than raising errors |

Source: `project/drivers/driver/python/S3_METADATA_IMPLEMENTATION.md` — "first-class metadata
restriction filtering."

---

## Recursive Schema Navigation

The `sys_information_recursive` profile supports recursive schema parent expansion. A schema
can be a child of another schema, and the driver can traverse the tree:

- `schema_paths_for_navigation(...)` — normalize, de-duplicate, and filter schema names;
  optionally enable parent expansion mode.
- `expand_schema_parent_paths(...)` — emit recursive dotted parent segments for
  metadata-only tree navigation.
- `build_schema_tree(...)` — construct a recursive `SchemaTreeNode` structure with
  per-parent uniqueness and terminal-node tracking.

The `metadata_expand_schema_parents` configuration option (available as a DSN key or
connection config alias) controls whether parent schemas are automatically expanded in
metadata queries. Recognized aliases include `metadataExpandSchemaParents`,
`metadata_expand_schema_parents`, `expand_schema_parents`, and `dbeaver_expand_schema_parents`.

Source: `project/drivers/driver/python/S3_METADATA_IMPLEMENTATION.md` — "recursive schema tree
shaping."

---

## Schema Navigation Payload

The `ddl_editor_schema_payload()` method returns a deterministic payload for tools that
display a schema tree (DDL editors, BI tools, IDE plugins). The payload contains:

- `schemaPaths` — a list of normalized schema path strings.
- `schemaTree` — a recursive tree of `SchemaTreeNode` objects.

Source: `project/drivers/driver/python/S3_METADATA_IMPLEMENTATION.md` — "deterministic
DDL-editor payload shaping."

---

## Query Resolution and Aliases

Metadata collection names are normalized before execution. The driver resolves aliases such
that a call to `.routines()` routes to the same metadata path as an explicit
`query_metadata('routines')`. Normalization also handles JDBC-style naming variations.

Source: `project/drivers/driver/python/S3_METADATA_IMPLEMENTATION.md` — "`normalize_collection_name(...)`",
"`resolve_collection_query(...)`."

---

## Cross-References

- [conformance_baseline.md](conformance_baseline.md) — META stage (S3) conformance requirement
- [Language Reference: Catalog Reference](../Language_Reference/catalog_reference/index.md) — canonical sys.information catalog structure
