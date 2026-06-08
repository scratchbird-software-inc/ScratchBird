# Firebird 5 exact rowset extraction status

Search key: `SB_REFERENCE_FIREBIRD_5_EXACT_ROWSET_EXTRACTION_STATUS`

Status: catalog and builtin inventory rowset hashes are executable CTest
evidence; object-level Firebird system rowset projection remains represented
as ScratchBird catalog-overlay emulation rows.

Evidence now copied into the private tree:

1. `project/tests/donor_regression/donor_emulation_source_summaries/firebird-5/50_catalog.md`.
2. `project/tests/donor_regression/donor_emulation_source_summaries/firebird-5/60_builtins.md`.

Evidence observed from Firebird metadata initialization code:

1. New database initialization stores system relations and relation fields from the internal relation/field arrays.
2. Initialization stores global fields after relation fields.
3. Initialization stores one `RDB$DATABASE` row.
4. Initialization stores system schemas.
5. Initialization stores `RDB$TYPES` rows from internal type arrays and character set type rows.
6. Initialization stores character set and collation symbols.
7. Initialization stores system generators.
8. Initialization stores privileges into `RDB$USER_PRIVILEGES` and security classes into `RDB$SECURITY_CLASSES`.
9. Monitoring tables are runtime/computed surfaces and must not be static seed rows.

Required hash outputs:

1. RDB$ catalog inventory rowset hash.
2. MON$ computed catalog surface hash.
3. builtin behavior inventory hash.

CTest evidence gate: `firebird_catalog_rowset_hash_gate`.

Catalog overlay mutation and verification are implemented through the
Firebird parser-support UDR installer state and the Firebird catalog-overlay
parser projection diagnostics.
