# MariaDB Exact Rowset Extraction Status

Search key: SB_REFERENCE_REFERENCE_MARIADB_EXACT_ROWSET_STATUS

## Status

MariaDB private reference material now identifies the main catalog, information-schema, plugin, datatype, and built-in behavior families. Exact default rowsets from a newly initialized MariaDB instance have not been captured.

The MariaDB reference pack is therefore not implementation-ready for catalog seeding.

## Required extraction profiles

The final seed material must include exact rowsets for:

- minimal newly initialized MariaDB server
- server with default plugins only
- server with `sys` schema installed
- server with performance schema enabled
- server with user statistics enabled
- server with wsrep/Galera enabled
- server with special datatype plugins enabled
- server with representative storage engines enabled

## Required generated-value classification

Every generated value must be classified as:

- stable reference alias over ScratchBird UUID
- deterministic reference profile value
- runtime server value
- runtime connection value
- runtime transaction value
- local metric projection from `sys.metrics.*`
- cluster metric projection from `cluster.sys.metrics.*` only when cluster exists
- redacted secret
- forbidden host-private value

## Diagnostics

| Condition | Diagnostic vector |
|---|---|
| Exact default rowset missing | REFERENCE.MARIADB.CATALOG.EXACT_ROWSET_MISSING |
| Catalog column omitted | REFERENCE.MARIADB.CATALOG.COLUMN_MISSING |
| Catalog column order unknown | REFERENCE.MARIADB.CATALOG.COLUMN_ORDER_UNKNOWN |
| SQL mode rowset delta unspecified | REFERENCE.MARIADB.CATALOG.SQL_MODE_DELTA_MISSING |
| Plugin rowset delta unspecified | REFERENCE.MARIADB.CATALOG.PLUGIN_DELTA_MISSING |
| Storage-engine rowset delta unspecified | REFERENCE.MARIADB.CATALOG.ENGINE_DELTA_MISSING |
| Raw secret copied into seed material | REFERENCE.MARIADB.CATALOG.RAW_SECRET_FORBIDDEN |
| Reference ID used as ScratchBird authority | REFERENCE.MARIADB.CATALOG.REFERENCE_ID_AUTHORITY_FORBIDDEN |
| Cluster surface exposed without cluster schema | REFERENCE.MARIADB.CATALOG.CLUSTER_SCHEMA_ABSENT |

## Conformance gates

| Gate | Required result |
|---|---|
| MARIADB-SEED-001 | Newly initialized MariaDB catalog rowsets match the private seed manifest after generated-value normalization. |
| MARIADB-SEED-002 | Plugin-enabled rowset deltas match the private plugin profile. |
| MARIADB-SEED-003 | Special datatype visibility and behavior match enabled plugin profile. |
| MARIADB-SEED-004 | Metadata visibility matches MariaDB privilege behavior for allowed and denied users. |
| MARIADB-SEED-005 | `sys.metrics.*` projections satisfy performance_schema and plugin report surfaces without exposing raw host paths. |
| MARIADB-SEED-006 | wsrep report surfaces are absent or disabled when no ScratchBird cluster exists. |
