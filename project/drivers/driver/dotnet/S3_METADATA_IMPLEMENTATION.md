# S3 Metadata Implementation (DLB-DOTNET-004)

Scope: `lanes/active/drivers/dotnet` lane only.

## Changes

- Added `MetadataExpandSchemaParents` to lane config with JDBC-compatible aliases:
  - `metadataExpandSchemaParents`
  - `metadata_expand_schema_parents`
  - `expandSchemaParents`
  - `expand_schema_parents`
  - `dbeaverExpandSchemaParents`
  - `dbeaver_expand_schema_parents`
- Updated `ScratchBirdConnection.GetSchema` metadata pipeline to:
  - normalize collection keys once;
  - shape metadata rows through a shared helper path;
  - apply restriction filtering for `Tables`, `Columns`, `Schemas`, and `Catalogs`;
  - expose additional metadata families (`Catalogs`, `PrimaryKeys`, `ForeignKeys`, `TablePrivileges`, `ColumnPrivileges`, `TypeInfo`, `Routines`);
  - optionally expand dotted schema parents for metadata-only recursive tree navigation when `MetadataExpandSchemaParents=true`.
- Extended restriction-column mapping across expanded metadata families in `GetSchema(collectionName, restrictionValues)`:
  - `Indexes`, `IndexColumns`, `Constraints`, `PrimaryKeys`, `ForeignKeys`, `TablePrivileges`, `ColumnPrivileges`, `Procedures`, `Functions`, `Routines`, and `TypeInfo`.
  - Added explicit `"null"` restriction-literal matching for nullable metadata fields.
- Added focused metadata shaping tests covering:
  - parent expansion ancestry/uniqueness behavior;
  - expansion + schema-pattern filtering behavior;
  - table/column restriction filtering behavior;
  - catalog restriction filtering and metadata collection alias normalization;
  - procedure/routine/type-info/primary-key restriction filtering for extended metadata families;
  - `"null"` restriction literal matching behavior.
- Added config parser tests verifying metadata parent-expansion aliases.
- Updated `BASELINE_REQUIREMENT_MAPPING.md` `META` row evidence/notes to reflect current behavior.

## Tests Run

- `dotnet test --filter "FullyQualifiedName~ScratchBirdConnectionMetadataShapingTests"`: **PASS** (18 passed, 0 failed, 0 skipped)
- `dotnet test tests/ScratchBird.Data.Tests/ScratchBird.Data.Tests.csproj --filter "FullyQualifiedName~ScratchBirdConnectionSchemaStatementTests"`: **PASS** (3 passed, 0 failed, 0 skipped)
- `dotnet test tests/ScratchBird.Data.Tests/ScratchBird.Data.Tests.csproj --filter "FullyQualifiedName~ConfigTests.ParseMetadataExpandSchemaParentsAliases"`: **PASS** (1 passed, 0 failed, 0 skipped)

## META Status Recommendation

- Recommendation: **PARTIAL**
- Reason: metadata shaping/family routing and restriction mapping are expanded, but live-metadata integration coverage and DDL-editor payload richness remain incomplete.

## Remaining Gaps

- Metadata field richness for DDL/editor parity is still incomplete in some collections.
- Parent-expanded schema rows are synthetic metadata rows (name-focused) and do not provide physical schema IDs for synthetic ancestors.
