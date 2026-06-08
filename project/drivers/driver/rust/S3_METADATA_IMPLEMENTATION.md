# DLB-RUST-004 S3 Metadata Implementation

Date: 2026-03-06  
Lane: `lanes/active/drivers/rust`

## What Changed

1. Extended metadata API surfaces on `Client` (`src/client.rs`):
   - `get_schema(collection, restrictions)` metadata-row map projection.
   - `get_schema_tree(schema_pattern, expand_parents)` deterministic recursive tree output.
   - `ddl_editor_schema_payload(schema_pattern, expand_schema_parents)` deterministic DDL-editor payload (`schemaPattern`, `expandSchemaParents`, `schemaPaths`, `schemaTree`).
2. Added schema-pattern matching helpers for DDL/editor navigation behavior:
   - case-insensitive SQL-like `%` / `_` matching with escape support.
3. Added config metadata recursion alias parsing in `src/config.rs`:
   - `metadata_expand_schema_parents` family (`metadataexpandschemaparents`, `expand_schema_parents`, `dbeaver_expand_schema_parents`, etc.).
4. Added deterministic always-on metadata runtime matrix tests in `tests/runtime_contract_gate_test.rs`:
   - collection coverage: catalogs, schemas, tables, columns, indexes, index_columns, constraints, primary_keys, foreign_keys, table_privileges, column_privileges, procedures, functions, routines, type_info.
   - restriction-family assertions plus DDL-editor payload and schema-tree coverage.

## Tests Run

1. `cargo test`
   - Result: `PASS`

## META Status Recommendation

Recommendation: `IMPLEMENTED` (baseline-complete for 0.1.0 scope).

