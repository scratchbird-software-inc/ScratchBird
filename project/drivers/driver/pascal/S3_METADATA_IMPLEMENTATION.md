# S3 META Implementation (DLB-PASCAL-004)

Scope: `lanes/active/drivers/pascal` only.

## What Changed

- Added metadata-only recursive schema shaping in `src/ScratchBird.Metadata.pas`:
  - Introduced metadata row model (`TMetadataField`, `TMetadataRow`, `TMetadataRows`) and case-insensitive field lookup (`MetadataRowTryGetValue`).
  - Expanded metadata collection/query coverage and alias normalization to include:
    - `catalogs`, `primary_keys`, `foreign_keys`,
    - `table_privileges`, `column_privileges`,
    - `routines`, `type_info`,
    alongside existing schema/table/column/index/constraint/procedure/function collections.
  - Switched procedure/function/routine metadata query builders to `information_schema.routines` so metadata execution avoids direct dependency on `sys.procedures`/`sys.functions` catalogs.
  - Expanded table/column/index/index-column/constraint/key/privilege metadata query builders with schema/table/index name joins and JDBC-style alias columns (`table_schema`, `table_schem`, `schema_name`) so restriction filters can operate on stable metadata names instead of ID-only payloads.
  - Added JDBC-oriented routine/type-info alias columns (`specific_name`, `type_name`, `data_type`) in routines/procedures/functions/type-info query builders to improve metadata shape parity without changing collection semantics.
  - Added `ExpandSchemaPaths` for dotted parent expansion with first-seen ordering and de-duplication.
  - Added `ListMetadataSchemaPaths` for schema-path extraction from metadata rows with optional parent expansion mode.
  - Added `ExpandSchemaMetadataRows` for metadata-row parent expansion that emits synthetic ancestor rows and preserves physical leaf rows.
  - Added `FilterMetadataRowsByRestrictions` for collection-scoped metadata row filtering with:
    - alias-based restriction key matching,
    - `%` / `_` wildcard matching semantics,
    - `null` literal handling for nullable-column restriction matching,
    - unsupported restriction-key ignore behavior.
  - Extended metadata restriction key routing so `routines` now accepts `procedure`/`function`-style restriction aliases and filters against `routine_name`.
  - Added `TMetadataSchemaTreeNode`/`TMetadataSchemaTree` plus `BuildMetadataSchemaTree` for recursive schema tree shaping with:
    - per-parent uniqueness semantics,
    - terminal-node tracking,
    - same leaf-name support under different parent branches,
    - optional database label on the output tree.
- Added focused FPC test program `tests/MetadataRecursiveSchemaTests.pas` that covers:
  - database/default branch-style metadata row expansion,
  - dotted parent expansion ordering and de-duplication,
  - per-parent uniqueness,
  - same leaf name under different parents,
  - metadata collection alias/query resolution for catalogs/keys/privileges/type/routines families,
  - restriction filtering behavior for aliases/wildcards/null semantics and unsupported key ignore behavior,
  - client metadata API guard behavior (`unsupported` -> `0A000`, disconnected supported collection -> `08003`).
- Added typed metadata wrapper methods on `TScratchBirdClient` for first-class metadata families:
  - `GetCatalogs`, `GetSchemas`, `GetTables`, `GetColumns`, `GetIndexes`, `GetIndexColumns`, `GetConstraints`,
  - `GetProcedures`, `GetFunctions`, `GetRoutines`,
  - `GetPrimaryKeys`, `GetForeignKeys`,
  - `GetTablePrivileges`, `GetColumnPrivileges`, `GetTypeInfo`.
- Added materialized metadata row APIs on `TScratchBirdClient`:
  - `QueryMetadataRows(collectionName, restrictions)`
  - `GetSchemaRows(collectionName, restrictions)`
  which execute metadata SQL, materialize `TMetadataRows`, and apply restriction filtering in-lane.
- Added adapter-level metadata API forwarding surfaces for:
  - `TScratchBirdFDConnection` (`ScratchBird.FireDAC.pas`)
  - `TScratchBirdIBDatabase` (`ScratchBird.IBX.pas`)
  - `TScratchBirdZConnection` (`ScratchBird.Zeos.pas`)
  - `TScratchBirdSQLConnection` (`ScratchBird.SQLdb.pas`)
  including:
  - generic metadata stream APIs (`QueryMetadata` / `GetSchema`),
  - materialized row APIs (`QueryMetadataRows` / `GetSchemaRows`, with restriction overloads),
  - typed metadata wrapper families (`GetCatalogs`, `GetSchemas`, `GetTables`, `GetColumns`, `GetIndexes`, `GetIndexColumns`, `GetConstraints`, `GetProcedures`, `GetFunctions`, `GetRoutines`, `GetPrimaryKeys`, `GetForeignKeys`, `GetTablePrivileges`, `GetColumnPrivileges`, `GetTypeInfo`).
- Added deterministic adapter metadata API guard suite:
  - `tests/AdapterMetadataApiTests.pas`
  - validates disconnected supported-collection behavior (`08003`) and unsupported collection behavior (`0A000`) across all four adapter surfaces.
- Added deterministic metadata execution-flow suite:
  - `tests/MetadataExecutionFlowTests.pas`
  - validates connected metadata wrapper query execution for all typed metadata wrappers:
    `catalogs`, `schemas`, `tables`, `columns`, `indexes`, `index_columns`, `constraints`,
    `procedures`, `functions`, `routines`, `primary_keys`, `foreign_keys`,
    `table_privileges`, `column_privileges`, and `type_info`.
  - validates restriction-aware `QueryMetadataRows(...)` materialization from wire row payloads for:
    - `tables`, `routines`,
    - `catalogs`, `columns`, `indexes`, `constraints`,
    - `primary_keys`, `foreign_keys`,
    - `table_privileges`, `column_privileges`,
    - `procedures`, `functions`, `type_info`.
- Expanded env-gated live metadata coverage in:
  - `tests/IntegrationTest.pas`
  - validates executable metadata stream paths for supported metadata families, including `catalogs`, `schemas`, `tables`, `columns`, `indexes`, `constraints`, `primary_keys`, `foreign_keys`, `table_privileges`, `column_privileges`, `procedures`, `functions`, `routines`, and `type_info`.
  - validates typed-wrapper metadata stream paths for the same family set.
  - validates restriction-aware `QueryMetadataRows(...)` materialization against a running endpoint across supported families (with family-local skip behavior when a collection has no rows or no compatible filter field in that fixture).
- Updated `BASELINE_REQUIREMENT_MAPPING.md` META evidence/notes for the new S3 metadata shaping coverage.

## Targeted Tests Run

1. `fpc -Mdelphi -Fu./lanes/active/drivers/pascal/src -FE./lanes/active/drivers/pascal/tests ./lanes/active/drivers/pascal/tests/MetadataRecursiveSchemaTests.pas`
- Result: PASS (compile succeeded).

2. `./lanes/active/drivers/pascal/tests/MetadataRecursiveSchemaTests`
- Result: PASS (`MetadataRecursiveSchemaTests: OK`).

3. `fpc -Mdelphi -Fu./lanes/active/drivers/pascal/src -FU/tmp/sb_pascal_meta_adapter_build -FE/tmp/sb_pascal_meta_adapter_bin ./lanes/active/drivers/pascal/tests/AdapterMetadataApiTests.pas`
- Result: PASS (compile succeeded).

4. `/tmp/sb_pascal_meta_adapter_bin/AdapterMetadataApiTests`
- Result: PASS (`AdapterMetadataApiTests: OK`).

5. `fpc -Mdelphi -Fu./lanes/active/drivers/pascal/src -FU/tmp/sb_pascal_meta_exec_build -FE/tmp/sb_pascal_meta_exec_bin ./lanes/active/drivers/pascal/tests/MetadataExecutionFlowTests.pas`
- Result: PASS (compile succeeded).

6. `/tmp/sb_pascal_meta_exec_bin/MetadataExecutionFlowTests`
- Result: PASS (`MetadataExecutionFlowTests: OK`).

7. `fpc -Mdelphi -Fu./lanes/active/drivers/pascal/src -FU/tmp/sb_pascal_next -FE/tmp/sb_pascal_next ./lanes/active/drivers/pascal/tests/IntegrationTest.pas`
- Result: PASS (compile succeeded).

8. `/tmp/sb_pascal_next/IntegrationTest`
- Result: PASS (`IntegrationTest: SKIPPED (SCRATCHBIRD_PASCAL_URL not set)` in non-env-gated local run).

## META Status Recommendation

- Recommendation: `PARTIAL`

Rationale:
- Metadata-only recursive schema shaping parity is now implemented in-lane and covered by deterministic lane tests for parent expansion and uniqueness semantics.
- Generic executable metadata APIs now exist on the client (`QueryMetadata` / `GetSchema`) with expanded metadata family coverage.
- Typed client metadata wrappers now exist for the expanded metadata family surface.
- Restriction-aware filtering parity now exists for materialized metadata rows (`FilterMetadataRowsByRestrictions` + `QueryMetadataRows`/`GetSchemaRows`).
- Deterministic metadata execution-flow coverage now validates wrapper query routing for schema/table/column/index/constraint/routine families and broader restriction-aware row materialization from wire payloads across additional metadata families.
- Adapter-level metadata forwarding surfaces now exist with deterministic lane-local guard coverage.
- Env-gated live integration now validates executable metadata stream and typed-wrapper paths across supported metadata families (catalogs/schemas/tables/columns/indexes/constraints/keys/privileges/procedures/functions/routines/type_info).
- Env-gated live integration now also validates restriction-aware `QueryMetadataRows(...)` materialization across supported metadata families.
- Status remains partial because live coverage is env-gated/skippable, live restriction checks still allow family-local skips when fixtures do not expose rows/fields, and JDBC result-shape parity is still incomplete.

## Remaining Concrete Gaps

- Add non-skippable gate execution for metadata live integration assertions.
- Replace family-local skip behavior in live restriction assertions with fixture-backed non-skippable cases.
- JDBC metadata result-shape parity remains incomplete for richer per-family columns/flags.
