# ScratchBird Driver Baseline Requirement Mapping (S0)

Scope: lane-local S0 artifact only for `lanes/active/drivers/r`.

## MGA Recovery Contract

- This lane follows ScratchBird's MGA/state-based engine recovery model.
- Reconnect or reopen only repairs transport and session state.
- Reconnect never resurrects abandoned in-flight transactions or replay lost statements.
- Transaction recovery in the lane means reset, rollback, reopen, or retry against engine truth.
- Result resume is valid only for explicit suspended protocol states.
- `sb_prepare_transaction(...)`, `sb_commit_prepared(...)`, and
  `sb_rollback_prepared(...)` expose prepared/limbo control SQL explicitly in
  lane source.
- `sb_supports_dormant_reattach(...)` is explicit and
  `sb_detach_to_dormant(...)` / `sb_reattach_dormant(...)` fail closed with
  `0A000` instead of treating reconnect as dormant resume.
- `sb_begin(...)` exposes the canonical MGA begin payload fields for
  `isolation_level`, `access_mode`, `deferrable`, `wait`, `timeout_ms`,
  `autocommit_mode`, `conflict_action`, and `read_committed_mode`.
- Native `READY`, `TXN_STATUS`, and `current_txn_id` are authoritative for
  transaction activity in this lane; ScratchBird sessions stay always in a
  transaction and `COMMIT` / `ROLLBACK` reopen the next boundary.
- `sb_begin(...)` is documented against that always-in-transaction contract
  rather than idle-session semantics.
- Result fetch now ignores one stray reopen `READY` before any real result
  material so the first post-commit / post-rollback query sees actual rows.
- `sb_canonical_isolation_label(...)` makes the current alias mapping explicit
  in lane source: `READ UNCOMMITTED` remains a legacy compatibility alias,
  `READ COMMITTED` => canonical `READ COMMITTED`,
  `REPEATABLE READ` => canonical `SNAPSHOT`,
  `SERIALIZABLE` => canonical `SNAPSHOT TABLE STABILITY`.
- `sb_canonical_read_committed_mode_label(...)` makes the canonical
  `READ COMMITTED` sub-mode selector explicit in lane source, including
  `READ COMMITTED READ CONSISTENCY`.
- `sb_retry_scope_for_sqlstate(...)` makes the retry boundary explicit:
  `40001`/`40P01` => fresh statement only, `08xxx` => reconnect or reopen
  only, everything else => no automatic replay.
- See `../../../../public_audit_summary`.

## CONN -> JDBCBL-CONN

- Current status: `Implemented`
- Lane-local source anchors:
  - `R/config.R:8` (`sb_config` defaults and DSN entrypoint)
  - `R/auth_bootstrap.R` (shared auth/bootstrap descriptors and token payload resolution)
  - `R/config.R:69` (`parse_uri_dsn`)
  - `R/config.R:105` (`parse_kv_dsn`)
  - `R/client.R` (`sb_connect`, `sb_probe_auth_surface`, `sb_get_resolved_auth_context`)
  - `R/client.R:241` (`sb_open_socket` transport open path)
  - `R/client.R` (`sb_perform_manager_connect`, manager-proxy auth flow + resolved ingress tracking)
  - `R/client.R` (`sb_probe_direct_auth_surface`, `sb_probe_manager_auth_surface`)
  - `R/client.R` (`sb_startup_and_auth`, direct auth execution for PASSWORD/SCRAM/TOKEN)
  - `R/protocol.R:155` (`read_u32` unsigned protocol length decode)
  - `R/protocol.R:222` / `R/protocol.R:229` / `R/protocol.R:239` (auth frame parsers)
  - `R/native_transport.R:9` (`sb_tls_connect_native` native TLS call bridge)
  - `R/dbi.R:16` / `R/dbi.R:23` / `R/dbi.R:35` / `R/dbi.R:40` (`dbConnect`, `dbCanConnect`, `dbDisconnect`, `dbIsValid`)
- Lane-local test anchors:
  - `tests/testthat/test_config.R:8`
  - `tests/testthat/test_config.R:22`
  - `tests/testthat/test_config.R:33`
  - `tests/testthat/test_auth_bootstrap.R`
  - `tests/testthat/test_conn_protocol.R:8`
  - `tests/testthat/test_conn_protocol.R:32`
  - `tests/testthat/test_conn_protocol.R:70`
  - `tests/testthat/test_conn_protocol.R:90`
  - `tests/testthat/test_conn_protocol.R:98`
  - `tests/testthat/test_conn_protocol.R:118`
  - `tests/testthat/test_conn_protocol.R:124`
  - `tests/testthat/test_transport_tls.R:9`
  - `tests/testthat/test_integration.R:54` (direct connect/query)
  - `tests/testthat/test_integration.R:63` (manager-proxy connect/query)
- Gaps / next actions:
  - Live connection/auth coverage remains environment-gated (`tests/testthat/test_integration.R` depends on `SCRATCHBIRD_R_URL` and `SCRATCHBIRD_R_MANAGER_URL`).
  - Maintain the current pkgload-based local suite path as a release gate so connection/auth regressions are caught even when live DSNs are absent.

## TXN -> JDBCBL-TXN

- Current status: `Implemented`
- Lane-local source anchors:
  - `R/client.R:86` (`sb_begin`)
  - `R/client.R` (`sb_transaction_active`, `sb_apply_runtime_txn_id`,
    `sb_apply_runtime_ready_state`, `sb_can_adopt_fresh_native_boundary`)
  - `R/client.R` (`sb_canonical_isolation_label`, explicit isolation alias mapping)
  - `R/client.R` (`sb_prepare_transaction`, `sb_commit_prepared`, `sb_rollback_prepared`)
  - `R/client.R` (`sb_supports_prepared_transactions`, `sb_supports_dormant_reattach`, dormant fail-closed helpers)
  - `R/client.R:113` (`sb_commit`)
  - `R/client.R:119` (`sb_rollback`)
  - `R/client.R:125` / `R/client.R:131` / `R/client.R:137` (savepoint operations)
  - `R/client.R:48` (`sb_set_autocommit`, local state toggle)
  - `R/dbi.R:44` / `R/dbi.R:50` / `R/dbi.R:56` (`dbBegin`, `dbCommit`, `dbRollback`)
- Lane-local test anchors:
  - `tests/testthat/test_txn_exec_parity.R:18` (DBI transaction lifecycle + autocommit alignment)
  - `tests/testthat/test_txn_exec_parity.R` (always-in-transaction `READY` / `TXN_STATUS` runtime-state proof)
  - `tests/testthat/test_txn_exec_parity.R` (canonical isolation alias helper coverage)
  - `tests/testthat/test_txn_exec_parity.R` (prepared/limbo control SQL + dormant fail-closed helpers)
  - `tests/testthat/test_txn_exec_parity.R:75` (wire message coverage for begin/commit/rollback/savepoint/release/rollback-to)
  - `tests/testthat/test_integration.R:81` (live DBI begin/rollback + autocommit recovery after error)
  - `tests/testthat/test_integration.R:98` (live savepoint/release/rollback-to lifecycle plus direct post-rollback query proof)
- Gaps / next actions:
  - Consider DBI-level savepoint helpers if S2 parity scope requires savepoint operations through DBI generic methods.

## EXEC -> JDBCBL-EXEC

- Current status: `Implemented`
- Lane-local source anchors:
  - `R/dbi.R:62` / `R/dbi.R:67` / `R/dbi.R:71` / `R/dbi.R:76` / `R/dbi.R:84` (DBI send/fetch/clear/rows-affected/execute methods)
  - `R/sql.R:8` / `R/sql.R:38` / `R/sql.R:76` (SQL normalization and placeholder rewrites)
  - `R/client.R:56` / `R/client.R:66` (`sb_query`, `sb_send_query`)
  - `R/client.R:493` (`sb_execute_query`)
  - `R/client.R` (`sb_allow_portal_resume`, `sb_resume_suspended_portal`, suspended-only portal resume guard)
  - `R/client.R` (`sb_prime_result_metadata`, `sb_result_next_row`, one-stray-reopen-READY ignore rule)
  - `R/client.R:544` (`sb_fetch_rows`)
  - `R/client.R:642` (extended query parse/bind/execute path)
  - `R/client.R:827` / `R/client.R:847` (`sb_rows_to_column` / `sb_rows_to_df`, typed row materialization)
- Lane-local test anchors:
  - `tests/testthat/test_sql.R:8`
  - `tests/testthat/test_sql.R:15`
  - `tests/testthat/test_sql.R:22`
  - `tests/testthat/test_txn_exec_parity.R:141` (`dbSendQuery` + `dbFetch` + `dbClearResult` lifecycle)
  - `tests/testthat/test_txn_exec_parity.R:182` (`dbExecute` rowcount + full drain behavior)
  - `tests/testthat/test_exec_lifecycle.R:34` (extended-query parse/bind/execute/sync order)
  - `tests/testthat/test_exec_lifecycle.R:72` (parameter-count mismatch fail-fast before bind/execute)
  - `tests/testthat/test_exec_lifecycle.R:102` (portal-suspended execute resume flow)
  - `tests/testthat/test_exec_lifecycle.R` (portal resume guard rejects unsuspended execution)
  - `tests/testthat/test_exec_lifecycle.R:144` (command-complete + ready terminal state shaping)
  - `tests/testthat/test_exec_lifecycle.R` (one-stray-reopen-READY ignore proof before result material)
  - `tests/testthat/test_integration.R:54` (live simple query)
  - `tests/testthat/test_integration.R:73` (live parameterized query)
  - `tests/testthat/test_integration.R:151` (live incremental fetch lifecycle with `fetch_size`)

## META -> JDBCBL-META

- Current status: `Partial`
- Lane-local source anchors:
  - `R/metadata.R:3` (`sb_metadata_schemas_query`)
  - `R/metadata.R:7` (`sb_metadata_tables_query`)
  - `R/metadata.R:11` (`sb_metadata_columns_query`)
  - `R/metadata.R:18` / `R/metadata.R:22` / `R/metadata.R:26` / `R/metadata.R:30` / `R/metadata.R:34` (indexes/constraints/procedures/functions)
  - `R/metadata.R:45` (`sb_metadata_schema_paths_for_navigation`, dotted parent expansion path shaping)
  - `R/metadata.R:69` (`sb_metadata_build_schema_tree`, recursive tree shaping with per-parent uniqueness)
  - `R/metadata.R:117` (`sb_metadata_build_schema_tree_rows`, database/default branch-style row shaping)
  - `R/dbi.R:71` (`dbColumnInfo`, result metadata projection with pre-fetch priming)
  - `R/dbi.R:120` (`dbListTables`, metadata-only table discovery with schema qualification)
  - `R/dbi.R:150` / `R/dbi.R:161` / `R/dbi.R:172` (`dbExistsTable` for `character`/`Id`/`SQL` names)
  - `R/dbi.R:183` / `R/dbi.R:228` / `R/dbi.R:273` (`dbListFields` for `character`/`Id`/`SQL` names)
  - `R/dbi.R:423` (`sb_metadata_tables_with_schema`, table->schema enrichment from metadata)
  - `R/dbi.R:456` (`sb_filter_tables_for_ref`, schema/table reference matching)
  - `R/client.R:510` (`sb_prime_result_metadata`, row-description priming before fetch)
  - `NAMESPACE:18` through `NAMESPACE:28` (metadata helper + recursive schema shaping exports)
- Lane-local test anchors:
  - `tests/testthat/test_metadata_recursive_schema.R:17` (database/default branch-style rows with top-level branches)
  - `tests/testthat/test_metadata_recursive_schema.R:32` (dotted parent expansion for schema navigation paths)
  - `tests/testthat/test_metadata_recursive_schema.R:44` (per-parent uniqueness for duplicate leaf paths)
  - `tests/testthat/test_metadata_recursive_schema.R:58` (same leaf name preserved under different parents)
  - `tests/testthat/test_metadata_execution.R:14` (`dbListTables` metadata-only listing behavior)
  - `tests/testthat/test_metadata_execution.R:41` (`dbExistsTable` metadata-only lookup behavior)
  - `tests/testthat/test_metadata_execution.R:69` (`dbListFields` metadata-only column listing behavior)
  - `tests/testthat/test_metadata_execution.R:121` (dbColumnInfo metadata priming + post-call fetch continuity)
  - `tests/testthat/test_metadata_execution.R:167` (dbColumnInfo empty-result projection shape)
  - `tests/testthat/test_integration.R:111` (live metadata query wrappers + schema tree row shaping)
  - `tests/testthat/test_integration.R:133` (live metadata wrapper family smoke coverage)
- Gaps / next actions:
  - Expand metadata-family coverage toward richer privilege/key/type and DDL-editor payload parity expectations.

## TYPE -> JDBCBL-TYPE

- Current status: `Implemented`
- Lane-local source anchors:
  - `R/types.R:88` (`encode_param`)
  - `R/types.R:174` (`decode_value`)
  - `R/types.R:184` (`decode_binary_value`)
  - `R/types.R:342` / `R/types.R:393` (range encode/decode)
  - `R/types.R:448` / `R/types.R:475` (composite encode/decode)
  - `R/client.R:556` (row decode calls `decode_value`)
- Lane-local test anchors:
  - `tests/testthat/test_types.R:8` (UUID decode)
  - `tests/testthat/test_types.R:14` (primitive scalar decode matrix)
  - `tests/testthat/test_types.R:29` (temporal + interval decode matrix)
  - `tests/testthat/test_types.R:47` (vector/range/composite decode matrix)
  - `tests/testthat/test_types.R:69` (`encode_param` primitive + wrapper dispatch)
  - `tests/testthat/test_types.R:98` (vector/array literal family coverage)
  - `tests/testthat/test_integration.R:194` (`type_coverage` fixture integration)

## ERR -> JDBCBL-ERR

- Current status: `Implemented`
- Lane-local source anchors:
  - `R/protocol.R:590` (`parse_error_message`)
  - `R/client.R:565` (`sb_sqlstate_error_class`)
  - `R/client.R:615` (`sb_raise_query_error`)
  - `R/client.R:518` / `R/client.R:680` (error handling in query/drain loops and describe flow)
  - `R/client.R:711` (parameter-count mismatch guard)
- Lane-local test anchors:
  - `tests/testthat/test_error_parity.R:25` (exact + class-prefix SQLSTATE mapping coverage)
  - `tests/testthat/test_error_parity.R:34` (typed condition class + SQLSTATE/detail/hint propagation)
  - `tests/testthat/test_error_parity.R:59` (unknown SQLSTATE class fallback)
  - `tests/testthat/test_error_parity.R:74` (empty-SQLSTATE generic fallback)
  - `tests/testthat/test_integration.R:185` (live syntax-error path)
  - `tests/testthat/test_integration.R:201` (live cancel path expects error)
  - `tests/testthat/test_config.R:40` (config validation error path)

## RES -> JDBCBL-RES

- Current status: `Implemented`
- Lane-local source anchors:
  - `R/dbi.R:23` (`dbDisconnect`)
  - `R/dbi.R:41` (`dbClearResult`)
  - `R/client.R:43` (`sb_disconnect`)
  - `R/client.R` (`sb_prepare_connection`, reconnect resets abandoned prepared/portal-resume state)
  - `R/client.R:76` (`sb_clear_result`)
  - `R/client.R:81` (`sb_cancel`)
  - `R/client.R:164` (`sb_terminate`)
  - `R/client.R:258` (`sb_socket_close`)
  - `src/tls_transport.c:78` / `src/tls_transport.c:490` (native transport finalizer and explicit close)
- Lane-local test anchors:
  - `tests/testthat/test_integration.R:176` (live ping roundtrip with follow-up query)
  - `tests/testthat/test_integration.R:185` (live post-error usability)
  - `tests/testthat/test_integration.R:201` (live cancel during long-running fetch path)
  - `tests/testthat/test_resilience_lifecycle.R:36` (`sb_disconnect` repeated-call behavior with non-NULL close followed by NULL-safe close handling).
  - `tests/testthat/test_resilience_lifecycle.R:62` (`dbDisconnect` repeated-call idempotence with stable TRUE return and one real close).
  - `tests/testthat/test_resilience_lifecycle.R:88` (`sb_prepare_connection` reconnect path clears abandoned transaction/prepared state before startup/auth re-entry).
  - `tests/testthat/test_resilience_lifecycle.R:156` (`dbClearResult` idempotent completion path).
  - `tests/testthat/test_resilience_lifecycle.R:166` deterministic server-error fetch path followed by explicit result cleanup.
