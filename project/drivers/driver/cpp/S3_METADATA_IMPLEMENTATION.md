# S3 Metadata Implementation (DLB-CPP-004)

## Scope

- Lane: `lanes/active/drivers/cpp`
- Focus: metadata collection routing + recursive schema shaping parity against JDBC baseline expectations.

## Changes

- Added lane-local metadata shaping API surface:
  - `include/scratchbird/client/metadata.h`
    - `metadataSchemaPathsForNavigation(...)` for optional dotted parent expansion.
    - `buildMetadataSchemaTree(...)` for recursive metadata-only schema tree construction.
    - `buildMetadataSchemaTreeRows(...)` for flattened metadata rows with database-root + schema-branch shape.
    - `normalizeMetadataCollectionName(...)` for metadata collection alias normalization.
    - `resolveMetadataCollectionQuery(...)` for executable metadata collection query resolution.
    - `metadataCollectionNotSupportedMessage(...)` for deterministic unsupported-collection errors.
- Added implementation in `src/metadata.cpp`:
  - Path normalization and dotted-segment parsing.
  - Parent expansion with insertion-order de-duplication.
  - Recursive tree build keyed by full path to preserve:
    - parent uniqueness (no duplicate child under one parent),
    - same-name leaf nodes under different parents as distinct nodes.
  - Metadata row shaping beginning from database root, then top-level schema branches and descendants.
  - Metadata collection query map/alias normalization for extended families:
    - catalogs
    - primary keys
    - foreign keys
    - table privileges
    - column privileges
    - type info
    - routines
- Added C API executable metadata surface in `include/scratchbird/client/scratchbird_client.h` + `src/scratchbird_client_c.cpp`:
  - `sb_metadata_query(...)` resolves metadata collection names and executes through the normal query path.
- Added focused S3 tests in `tests/test_metadata_schema_tree.cpp`:
  - `TreeRowsStartAtDatabaseAndExposeTopBranches`
  - `ParentExpansionAddsDottedSchemaAncestors`
  - `ParentDoesNotAllowDuplicateChildNames`
  - `SameLeafNameUnderDifferentParentsIsPreserved`
  - `NormalizesCollectionAliasesForExtendedFamilies`
  - `ResolvesExtendedCollectionQueries`
  - `RejectsUnsupportedCollection`
- Added C API guardrail coverage in `tests/test_type_mapping.cpp`:
  - `MetadataQueryRequiresConnectionHandle`
- Wired source/test into build in `CMakeLists.txt`.
- Updated `BASELINE_REQUIREMENT_MAPPING.md` META evidence and remaining-gap notes.

## Test Commands Run

1. `cmake --build build_odbc_gate -j4`
   - Result: `PASS`
2. `cmake --build build_cpp_meta -j4`
   - Result: `PASS`
3. `ctest --test-dir build_cpp_meta --output-on-failure`
   - Result: `PASS` (`scratchbird_client_tests`)

## META Status Recommendation

- Recommendation: `PARTIAL`
- Reason:
  - Implemented and tested executable metadata collection normalization/query resolution plus recursive schema shaping behaviors.
  - Added C API metadata query routing surface (`sb_metadata_query`) to execute resolved metadata collections through the normal query path.
  - Lane remains partial pending richer live integration assertions and DDL-editor payload completeness validation.

## Remaining Gaps

- Add live metadata query + `sb_get_column_meta` verification using concrete metadata result flows against a running endpoint.
- Add DDL-editor field-completeness coverage beyond schema-tree shaping behavior.
