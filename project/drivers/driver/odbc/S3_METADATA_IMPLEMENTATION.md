# S3 Metadata Implementation (DLB-ODBC-004)

Scope: `lanes/active/drivers/odbc` lane only.

## Changes

1. Added metadata-only recursive schema shaping helpers.
   - `include/scratchbird/odbc/metadata_helpers.h:36`
     - `MetadataSchemaTreeNode`, `MetadataSchemaTree`, and `MetadataSchemaTreeRow` models.
   - `include/scratchbird/odbc/metadata_helpers.h:147`
     - `metadataSchemaPathsForNavigation(...)` for dotted parent expansion with insertion-order de-duplication.
   - `include/scratchbird/odbc/metadata_helpers.h:181`
     - `buildMetadataSchemaTree(...)` for recursive tree shaping with per-parent uniqueness.
   - `include/scratchbird/odbc/metadata_helpers.h:267`
     - `metadataSchemaChildren(...)` for per-parent child branch resolution.
   - `include/scratchbird/odbc/metadata_helpers.h:293`
     - `buildDatabaseDefaultMetadataRows(...)` for database-root plus branch-style metadata row shaping.

2. Wired browse metadata path handling to recursive schema traversal.
   - `src/odbc_handles.cpp:510`
     - `splitBrowsePath(...)` now prefers slash-delimited path splitting so dotted schema segments are preserved.
   - `src/odbc_handles.cpp:2107`
     - Catalog->schema stage now shapes metadata schemas into recursive top-level branches.
   - `src/odbc_handles.cpp:2130`
     - Schema stage now returns child schema branches for non-leaf parents; falls through to tables only at leaf nodes.
   - `src/odbc_handles.cpp:2162`
     - Leaf table listing now applies schema filtering to keep parent context isolation.

3. Added focused tests for required S3 behaviors.
   - `tests/test_odbc_capabilities_browse.cpp:232`
     - `OdbcMetadataShapingTest.DatabaseDefaultRowsExposeDefaultBranchPaths`
   - `tests/test_odbc_capabilities_browse.cpp:263`
     - `OdbcMetadataShapingTest.ParentExpansionAndPerParentUniquenessAreStable`
   - `tests/test_odbc_capabilities_browse.cpp:289`
     - `OdbcMetadataShapingTest.SameLeafNameUnderDifferentParentsIsDistinct`
   - `tests/test_odbc_capabilities_browse.cpp:362`
     - `OdbcCapabilityBrowseTest.BrowseConnectExpandsRecursiveSchemaBranchesBeforeTables`
   - `tests/test_odbc_capabilities_browse.cpp:403`
     - `OdbcCapabilityBrowseTest.BrowseConnectDeduplicatesSiblingLeavesAndKeepsParentIdentity`
   - `tests/test_odbc_capabilities_browse.cpp:519`
     - `OdbcCapabilityBrowseTest.BrowseConnectPathFallbackPreservesDottedSchemaSegments`

4. Updated baseline mapping evidence and status.
   - `BASELINE_REQUIREMENT_MAPPING.md:14`
     - `META` row updated to `Partial` with S3 recursive-schema evidence and remaining-gap note.

## Test Commands Run

- `cmake --build build --target scratchbird_odbc_tests -j 4`
  - Result: PASS
- `./build/lanes/active/drivers/odbc/scratchbird_odbc_tests --gtest_filter='OdbcMetadataShapingTest.*:OdbcCapabilityBrowseTest.BrowseConnect*'`
  - Result: PASS (10 passed, 0 failed)

## META Status Recommendation

- Recommendation: `PARTIAL`
- Reason:
  - Recursive schema tree parity behaviors for metadata-only shaping are now implemented and tested (database->default branches, dotted parent expansion, per-parent uniqueness, and same leaf names under different parents).
  - Lane still does not cover the full executable metadata family parity surface required for a complete `JDBCBL-META` `MET` determination.
