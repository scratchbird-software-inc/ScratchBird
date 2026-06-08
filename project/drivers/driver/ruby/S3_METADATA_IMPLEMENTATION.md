# DLB-RUBY-004 S3 Metadata Implementation

Date: 2026-03-04
Lane: `lanes/active/drivers/ruby`
Scope: metadata recursive schema shaping plus first-class restriction-aware metadata querying.

## Changes Implemented

1. Metadata recursive schema shaping helpers
   - File: `lib/scratchbird/metadata.rb`
   - Added:
     - `SchemaTreeNode` for recursive metadata tree nodes (`name`, `full_path`, `terminal`, `children`)
     - `schema_paths_for_navigation(..., expand_schema_parents:)` for normalized/de-duplicated schema path extraction with optional parent expansion
     - `expand_schema_parent_paths(...)` for dotted parent expansion (`users.alice.dev` -> `users`, `users.alice`, `users.alice.dev`)
     - `build_schema_tree(...)` for recursive schema-tree shaping with per-parent uniqueness and terminal tracking
     - `expand_schema_metadata_rows(...)` for synthetic ancestor metadata rows when parent expansion is enabled
     - `build_database_default_metadata_rows(...)` for database->default branch-style metadata row shaping from metadata-only schema paths
   - Effect:
     - Parent expansion is now an explicit option at metadata shaping time.
     - Tree shaping preserves recursive ancestry.
     - Duplicate siblings under the same parent are de-duplicated.
     - Same object names under different parents remain distinct nodes.

2. First-class metadata restriction filtering
   - Files:
     - `lib/scratchbird/metadata.rb`
     - `lib/scratchbird/client.rb`
     - `lib/scratchbird/connection.rb`
   - Added:
     - `Metadata.normalize_restrictions(...)`
     - `Metadata.filter_rows_by_restrictions(..., collection_name:)`
     - restriction alias maps across schema/table/column/index/constraint/procedure/function/type families
     - `Client#query_metadata_with_restrictions(...)`
     - `Client#get_schema_with_restrictions(...)`
     - `Connection#query_metadata_with_restrictions(...)`
     - `Connection#get_schema_with_restrictions(...)`
     - optional `restrictions:` keyword on `get_schema_tree(...)` (client + connection)
   - Effect:
     - Restriction filtering now executes inside the driver metadata layer rather than requiring consumers to filter rows manually.
     - Unknown/unmappable restrictions are ignored.
     - `"null"` restriction values match metadata `nil` values.
     - Existing metadata methods remain backward compatible.

3. Focused metadata tests for required S3 cases
   - File: `test/test_metadata_recursive_schema.rb`
   - Added coverage:
     - database->default branch style metadata rows
     - dotted schema parent expansion
     - uniqueness within a parent
     - same object name under different parents allowed

4. Restriction-aware metadata execution tests
   - File: `test/test_metadata_execution.rb`
   - Added coverage:
     - collection alias routing remains intact
     - restriction filtering by family aliases (`schema`, `table`)
     - null matching via `"null"`
     - unknown restriction key ignore behavior
     - `get_schema_with_restrictions(...)` filter + parent expansion
     - connection forwarding for `get_schema_with_restrictions(...)`

5. Baseline mapping updates
   - File: `BASELINE_REQUIREMENT_MAPPING.md`
   - Updated `META` row source/test anchors and notes to reflect recursive shaping plus restriction-aware metadata querying evidence.

## Targeted Tests Run

1. `ruby -Itest test/test_metadata_execution.rb`
   - Result: PASS
   - Output summary: `9 runs, 17 assertions, 0 failures, 0 errors, 0 skips`

2. `ruby -Itest test/test_metadata_recursive_schema.rb`
   - Result: PASS
   - Output summary: `4 runs, 17 assertions, 0 failures, 0 errors, 0 skips`

3. `ruby -Itest test/test_sql.rb`
   - Result: PASS
   - Output summary: `5 runs, 9 assertions, 0 failures, 0 errors, 0 skips`

4. `ruby -Itest test/test_txn_exec_parity.rb`
   - Result: PASS
   - Output summary: `9 runs, 48 assertions, 0 failures, 0 errors, 0 skips`

5. `ruby -Itest test/test_conn_auth_protocol.rb`
   - Result: PASS
   - Output summary: `10 runs, 25 assertions, 0 failures, 0 errors, 0 skips`

6. Full lane unit suite (integration env-gated tests remain skipped without env vars)
   - Command: `ruby -Itest -e 'Dir["test/test_*.rb"].sort.each { |f| system("ruby -Itest #{f}") or exit(1) }'`
   - Result: PASS (`test_integration.rb` skipped as expected)

## META Status Recommendation

- Recommendation: `PARTIAL`
- Rationale:
  - Recursive schema tree parity behavior is implemented and unit-tested in-lane (parent expansion option, parent uniqueness, cross-parent same-name identity, and branch-style row shaping).
  - Driver-level restriction-aware metadata query surfaces are now implemented and unit-tested (`query_metadata_with_restrictions`, `get_schema_with_restrictions`, `restrictions:` for schema tree calls).
  - Lane still lacks full executable metadata API coverage across all JDBCBL-META families and live integration assertions.

## Remaining Gaps

1. Expand metadata family coverage (catalog/key/privilege/type and richer DDL editor payload fields).
2. Add stronger collection-specific restriction semantics beyond current generic alias mapping.
3. Add integration fixtures that validate metadata query payload/shape against a live catalog.

## Blockers

- None encountered for `DLB-RUBY-004` implementation and targeted test execution.
