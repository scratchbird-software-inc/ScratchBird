# S3 Metadata Implementation (DLB-GO-004)

Scope: `lanes/active/drivers/go` lane only.

## Changes

- Metadata resolution and filtering surfaces are implemented in `metadata.go` and `conn.go`:
  - Collection alias normalization (`NormalizeMetadataCollectionName`).
  - Collection query resolution (`ResolveMetadataCollectionQuery`).
  - Restriction-aware filtering (`filterMetadataRowsByRestrictions`) with alias families, null matching, and unknown-key ignore behavior.
  - Driver APIs `Conn.QueryMetadata(...)` and `Conn.QueryMetadataWithRestrictions(...)`.
- Added in-memory metadata row wrapper in `metadata_rows.go` for restriction-filtered results.
- Config alias support for schema-parent expansion remains in `config.go` (`metadata_expand_schema_parents` family).
- Added always-on runtime metadata contract gate in `runtime_contract_gate_test.go`:
  - Metadata query execution against a local scripted server without environment dependencies.

## Tests Run

- `cd lanes/active/drivers/go && go test ./...`
  - Result: `PASS`

## META Status Recommendation

- Recommendation: `IMPLEMENTED` (baseline-complete for the 0.1.0 scope).

