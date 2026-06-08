# DLB-NODE-004 S3 Metadata Implementation

## Changes
- Added a lane-local metadata API surface on `Client`:
  - `queryMetadata(collectionName, restrictions?)` and `getSchema(collectionName, restrictions?)` for supported metadata collections (`catalogs`, `schemas`, `tables`, `columns`, `indexes`, `index_columns`, `constraints`, `primary_keys`, `foreign_keys`, `table_privileges`, `column_privileges`, `procedures`, `functions`, `type_info`).
  - `catalogs` is served locally from configured database context for deterministic metadata availability.
  - Unsupported collections now fail deterministically with `ScratchbirdNotSupportedError` (`0A000`).
  - `getSchema("schemas")` now supports JDBC-like parent expansion when `metadataExpandSchemaParents` is enabled.
- Added metadata helper utilities in `src/metadata.ts`:
  - Collection name normalization and SQL resolver.
  - Restriction normalization/filtering (`normalizeMetadataRestrictions`, `filterMetadataRowsByRestrictions`) with alias matching, null matching, and unknown-key ignore behavior.
  - Recursive schema parent expansion helper.
  - Metadata-only recursive schema tree builder (`buildMetadataSchemaTree`) with parent uniqueness and terminal-node tracking.
- Expanded metadata SQL payload depth for JDBC/ODBC-style consumers:
  - Added schema/table joins for table/column/index/constraint/privilege/routine families.
  - Added row-shaping utility (`shapeMetadataRowsForCollection`) to surface compatibility aliases (`TABLE_CAT`, `TABLE_SCHEM`, `TABLE_NAME`, `COLUMN_NAME`, `TYPE_NAME`, `DATA_TYPE`, and related collection-specific fields).
- Added DSN/config parity for parent expansion mode:
  - New `ClientConfig.metadataExpandSchemaParents`.
  - DSN aliases: `metadataExpandSchemaParents`, `metadata_expand_schema_parents`, `expandSchemaParents`, `expand_schema_parents`, `dbeaver_expand_schema_parents`.
- Added targeted lane unit coverage for metadata behavior and recursive schema tree shaping, including restriction-filter behavior and restricted schema expansion order.

## Tests Run
- `npm run build && node --test test/unit.test.js` -> PASS
  - 17 tests passed, 0 failed.
- `npm test` -> PASS
  - Integration suite includes env-gated metadata helper assertions for JDBC-compatible alias fields.

## META Status Recommendation
- Recommendation: `IMPLEMENTED`
- Why:
  - Implemented and tested: metadata collection routing across core plus catalog/key/privilege/type families, first-class restriction-aware filtering, recursive schema ancestry preservation, metadata-only tree shaping, per-parent uniqueness, schema/table join depth, JDBC-compatible alias shaping, config/DSN parent-expansion mode, and env-gated metadata integration assertions.

## Remaining Gaps
- None for baseline `JDBCBL-META` scope.
