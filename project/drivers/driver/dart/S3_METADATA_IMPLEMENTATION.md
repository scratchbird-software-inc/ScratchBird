# DLB-DART-004 S3 Metadata Implementation

## Scope

- Lane: `lanes/active/drivers/dart`
- Focus: metadata-only recursive schema shaping parity with JDBC baseline behavior.

## Changes Applied

1. Metadata shaping helpers and collection resolver (`lib/src/metadata.dart`)
- Added collection normalization + SQL resolver helpers:
  - `MetadataCollectionName`
  - `normalizeMetadataCollectionName(...)`
  - `resolveMetadataCollectionQuery(...)`
- Added metadata-only recursive schema APIs:
  - `expandSchemaPaths(...)`
  - `listMetadataSchemaPaths(...)`
  - `buildMetadataSchemaTree(...)`
  - `MetadataSchemaTree` and `MetadataSchemaTreeNode`
- Added metadata row shaping with optional parent expansion:
  - `expandSchemaMetadataRows(...)`
  - Synthetic parent rows preserve catalog fields (`TABLE_CATALOG`/`TABLE_CAT`/`database`) while nulling non-schema metadata columns.

2. Recursive schema behavior parity
- Parent expansion is optional via `expandParents` flags.
- Dotted ancestry is preserved in insertion order.
- Per-parent uniqueness is enforced via path-based node identity.
- Same leaf names under different parent branches remain distinct nodes.

3. Focused metadata tests (`test/metadata_recursive_schema_test.dart`)
- Added coverage for:
  - database->default-branch style metadata rows,
  - dotted schema parent expansion,
  - uniqueness within same parent,
  - same leaf name under different parents.

4. Baseline mapping refresh
- Updated `BASELINE_REQUIREMENT_MAPPING.md` META row source/test anchors and notes for new recursive metadata shaping evidence.

## Targeted Tests Run

1. `dart test test/metadata_recursive_schema_test.dart`
- Result: `PASS` (`4` tests passed)

2. `dart test test/config_test.dart test/type_mapping_test.dart test/txn_exec_parity_test.dart`
- Result: `PASS` (`18` tests passed)

## META Status Recommendation

- Recommendation: `PARTIAL`

## Rationale

- Implemented and tested: metadata-only recursive schema shaping, optional parent expansion, per-parent uniqueness, and same-name cross-parent leaf preservation.
- Metadata helper surface is stronger and now includes explicit schema-tree and metadata-row shaping utilities.
- Status remains partial because client-level metadata execution APIs and live metadata integration coverage are still incomplete relative to full `JDBCBL-META` scope.

## Remaining Gaps

- No client-facing metadata execution methods currently wire these shaping helpers end-to-end (`getSchema`/`getSchemaTree`-equivalent APIs).
- No runtime config/DSN toggle currently maps to metadata parent expansion mode in this lane.
- No live integration metadata validation for full DDL editor payload families (catalog/key/privilege/type and richer metadata surfaces).
