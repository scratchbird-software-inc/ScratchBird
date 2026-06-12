# MySQL 8.4 builtin mapping status

Search key: `SB_REFERENCE_MYSQL_8_4_BUILTIN_MAPPING_STATUS`

Status: incomplete, mapping table required before implementation-ready function/operator/routine emulation.

Private evidence now available:

1. `project/tests/reference_regression/reference_emulation_source_summaries/mysql-8.4/60_builtins.md`.
2. `project/tests/reference_regression/reference_catalog_seeds/mysql_mariadb/mysql_8_4_builtin_inventory.csv`.

Current extracted inventory status:

1. The copied summary contains a large list of built-in SQL functions from MySQL's function factory source.
2. Each extracted function currently has `requires_native_v3_sblr_mapping` status.
3. Exact argument arity, variadic behavior, SQL mode dependency, determinism, collation effects, temporal/timezone effects, JSON path behavior, spatial behavior, lock/session side effects, GTID/replication effects, and diagnostics still need normalized mapping rows.

Required mapping groups:

1. Numeric and math functions.
2. Text, charset, collation, and binary/string functions.
3. Temporal functions and SQL-mode-dependent date/time behavior.
4. JSON functions and JSON path behavior.
5. Spatial and MBR/ST_* functions.
6. Network and UUID functions.
7. Compression, hash, crypto, and random functions.
8. Lock/session/status functions such as `GET_LOCK`, `RELEASE_LOCK`, `CONNECTION_ID`, and performance-schema helpers.
9. Replication/GTID wait and comparison functions, all migration/report-only unless a native v3 mapping is explicitly defined.
10. File/external functions such as `LOAD_FILE`, requiring sandbox or deterministic refusal.

Invalid implementation-ready claim diagnostic: `MYSQL_MARIADB.FUNCTION_MAPPING_MISSING`.
