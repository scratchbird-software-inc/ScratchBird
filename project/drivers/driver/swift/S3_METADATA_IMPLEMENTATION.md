# DLB-SWIFT-004 S3 Metadata Implementation

## Scope

- Lane: `lanes/active/drivers/swift`
- Focus: metadata-only recursive schema shaping parity with JDBC baseline behavior.

## Changes Applied

1. Metadata recursive schema shaping (`Sources/ScratchBird/Metadata.swift`)
- Added metadata schema tree model:
  - `ScratchBirdMetadataSchemaTreeNode`
  - `ScratchBirdMetadataSchemaTree`
- Added metadata row model for schema navigation surfaces:
  - `ScratchBirdMetadataTreeRowKind`
  - `ScratchBirdMetadataSchemaTreeRow`
- Added metadata-only shaping helpers:
  - `splitMetadataSchemaPath(...)`
  - `normalizeMetadataSchemaPath(...)`
  - `metadataSchemaPathsForNavigation(...)`
  - `buildMetadataSchemaTree(...)`
  - `buildMetadataSchemaTreeRows(...)`
- Behavior added:
  - Optional dotted parent expansion (`expandSchemaParents`).
  - Path-keyed node construction to enforce uniqueness within the same parent branch.
  - Preservation of same-name leaves under different parents via full-path identity.
  - Database/default root metadata row shaping (`default` when database is blank).

2. Focused metadata tests (`Tests/ScratchBirdTests/MetadataRecursiveSchemaTests.swift`)
- Added coverage for:
  - database/default branch-style metadata rows,
  - dotted parent expansion,
  - uniqueness within same parent,
  - same leaf under different parents allowed.

3. Baseline mapping refresh
- Updated `BASELINE_REQUIREMENT_MAPPING.md` META source/test evidence and remaining-gap notes for this S3 work.

## Targeted Tests Run

1. `swift test --filter MetadataRecursiveSchemaTests`
- Result: `PASS` (`4` tests passed, `0` failed)

## META Status Recommendation

- Recommendation: `Partial`

## Rationale

- Implemented and tested metadata-only recursive schema shaping with optional parent expansion.
- Added explicit evidence for per-parent uniqueness and cross-parent same-name leaf preservation.
- Status remains partial because lane-level client metadata execution APIs and live engine metadata integration coverage are still incomplete for full `JDBCBL-META` parity.

## Remaining Gaps

- Add client-facing metadata query wrappers that route metadata collections through shaping helpers.
- Add live metadata integration tests validating engine-backed metadata payload completeness beyond recursive schema tree behavior.
