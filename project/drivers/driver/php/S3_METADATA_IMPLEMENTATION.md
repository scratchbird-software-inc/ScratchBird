# DLB-PHP-004 S3 Metadata Implementation

Date: 2026-03-04  
Lane: `lanes/active/drivers/php`

## What Changed

1. Added metadata-only recursive schema shaping helpers in `src/Metadata.php`:
   - `schemaPathsForNavigation(...)` and `expandSchemaPaths(...)` for normalized schema path handling and dotted parent expansion.
   - `listMetadataSchemaPaths(...)` for metadata-row schema path extraction plus optional parent expansion mode.
   - `buildMetadataSchemaTree(...)` for recursive schema tree generation with:
     - optional parent expansion,
     - per-parent uniqueness,
     - same leaf-name support under different parent paths,
     - optional database label on output.
   - `expandSchemaMetadataRows(...)` for metadata-row parent expansion that emits synthetic ancestor rows while preserving physical leaf rows.
2. Extended executable metadata collection routing in `src/Metadata.php`:
   - Added query constants and resolvers for extended metadata families:
     - `catalogs`
     - `primary_keys`
     - `foreign_keys`
     - `table_privileges`
     - `column_privileges`
     - `type_info`
     - `routines`
   - Expanded alias normalization (including separator/case variants like `primaryKeys`, `table privileges`, `column-privileges`).
3. Added focused lane tests in `tests/MetadataRecursiveSchemaTest.php` covering:
   - database/default branch-style metadata rows,
   - dotted parent expansion,
   - no duplicates within the same parent,
   - same leaf name under different parents.
4. Added metadata execution tests in `tests/MetadataExecutionTest.php` covering:
   - extended alias normalization,
   - extended collection query resolution,
   - connection-level metadata execution path (`Connection::queryMetadata(...)` / `Connection::getSchema(...)`) with wire-fixture validation of emitted metadata SQL,
   - restriction-aware metadata filtering (`Metadata::filterRowsByRestrictions(...)`) with alias matching, null matching, and unknown-key ignore behavior,
   - wire-level restriction filtering behavior through `Connection::getSchema(..., $restrictions)`,
   - unsupported collection mapping to `ScratchBirdNotSupportedException` (`0A000`).
5. Added first-class restriction filtering in `src/Metadata.php` and `src/Connection.php`:
   - `Metadata::normalizeRestrictions(...)`
   - `Metadata::filterRowsByRestrictions(...)`
   - alias/collection restriction key maps for metadata families
   - `Connection::queryMetadata(...)` plus optional restriction-aware filtering in `Connection::getSchema(...)` and `Connection::getSchemaTree(...)`.
6. Updated `BASELINE_REQUIREMENT_MAPPING.md` META evidence anchors and status note.

## Tests Run

1. `vendor/bin/phpunit --bootstrap tests/bootstrap.php tests/MetadataRecursiveSchemaTest.php tests/MetadataExecutionTest.php`
   Result: PASS

## META Status Recommendation

Recommendation: `Implemented`

Why:
- The lane has executable metadata collection routing and validation for extended metadata families with wire-level execution-path tests.
- First-class restriction-aware metadata filtering is implemented and covered in-lane.
- Recursive schema-tree shaping behavior is covered with dedicated tests and env-gated live metadata shape assertions.

## Blockers

1. None for current JDBC baseline scope.
