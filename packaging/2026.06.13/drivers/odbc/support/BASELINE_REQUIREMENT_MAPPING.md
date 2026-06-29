# ODBC Baseline Requirement Mapping (S0)

Scope: `lanes/active/drivers/odbc` lane only.

Status legend:
- `Implemented`: source surface is present with direct lane test anchors.
- `Partial`: source surface is present but has a known gap in current implementation.

## MGA Recovery Contract

- This lane follows ScratchBird's MGA/state-based engine recovery model.
- Reconnect or reopen only repairs transport and session state.
- Reconnect never resurrects abandoned in-flight transactions or replay lost statements.
- Transaction recovery in the lane means reset, rollback, reopen, or retry against engine truth.
- Result resume is valid only for explicit suspended protocol states.
- Disconnect clears statement handles, prepared SQL cache, transaction flags,
  and bridge session state before any later reconnect.
- The focused live recovery closure test in
  `tests/test_odbc_external_runtime.cpp` now proves that rollback leaves the
  next query immediately usable on the reopened native MGA boundary with no
  reconnect and no statement replay.
- `SQL_ATTR_TXN_ISOLATION` still exposes only ODBC's standard isolation flags;
  the lane now documents their canonical MGA meaning directly in source:
  `READ UNCOMMITTED` remains a legacy compatibility alias,
  `READ COMMITTED` => canonical `READ COMMITTED`,
  `REPEATABLE READ` and `VERSIONING` => canonical `SNAPSHOT`,
  `SERIALIZABLE` => canonical `SNAPSHOT TABLE STABILITY`.
- A distinct public selector for `READ COMMITTED READ CONSISTENCY` is not yet
  exposed through the ODBC transaction attribute surface.
- Retry remains SQLSTATE-driven and fail-closed in this lane:
  `40001`/`40P01` require a fresh statement boundary,
  `08xxx` requires reconnect or reopen, and nothing auto-replays a whole
  transaction.
- The generic ODBC retry helper now follows that same boundary and no longer
  retries operator-intervention/governance diagnostics such as `57014`.
- The shared staged auth/bootstrap contract is now explicit in the lane:
  `ConnectionParams` carries generic `auth_token`,
  connection-string / DSN parsing normalizes `AuthToken` / `BearerToken` /
  `Token`,
  `OdbcClientBridge::probeAuthSurface(...)` exposes pre-connect auth probing,
  and `getResolvedAuthContext()` preserves post-probe / post-connect auth truth.
- See `../../../../public_audit_summary`.

| ODBCBL group | JDBC baseline group | Current status | Evidence anchors (lane source/tests) |
| --- | --- | --- | --- |
| `CONN` | `JDBCBL-CONN` (connection/session lifecycle) | `Implemented` | `include/scratchbird/odbc/odbc_client_bridge.h` (`probeAuthSurface`, `getResolvedAuthContext`), `include/scratchbird/odbc/odbc_types.h` (`ConnectionParams::auth_token`), `src/odbc_client_bridge.cpp`, `src/odbc_handles.cpp` (`parseConnectionString`, `applyDsnConfig` auth-token normalization), `src/odbc_driver.cpp:235`, `src/odbc_driver.cpp:251`, `src/odbc_handles.cpp:1790`, `tests/test_odbc_catalog_and_types.cpp` (`OdbcAuthBootstrapTest.*`), `tests/test_odbc_driver_integration.cpp:150`, `tests/test_odbc_external_runtime.cpp:77` |
| `TXN` | `JDBCBL-TXN` (autocommit, isolation, commit/rollback) | `Implemented` | `include/scratchbird/odbc/odbc_handles.h` (prepared/dormant/no-portal capability declarations and wrapper-vs-native transaction-state note), `src/odbc_driver.cpp:890` (`SQLEndTran`), `src/odbc_handles.cpp:1774` (`OdbcEnvironment::endTransaction`), `src/odbc_handles.cpp:882` (`buildIsolationSql`, canonical isolation alias comments), `src/odbc_handles.cpp:1733` (`supportsPreparedTransactions`, `supportsDormantReattach`, `supportsPortalResume`, `buildPreparedTransactionSql`, `rejectDormantReattach`), `src/odbc_handles.cpp:2389` (`OdbcConnection::endTransaction`), `tests/test_odbc_catalog_and_types.cpp:500`, `tests/test_odbc_catalog_and_types.cpp:530`, `tests/test_odbc_catalog_and_types.cpp:551`, `tests/test_odbc_catalog_and_types.cpp` (`IsolationSqlDocumentsCanonicalAliasSurface`, `PreparedDormantAndPortalCapabilitiesStayExplicit`), `tests/test_odbc_external_runtime.cpp` (`RollbackLeavesImmediateQueryUsableOnFreshBoundary`) |
| `EXEC` | `JDBCBL-EXEC` (statement execution, binding, fetch) | `Implemented` | `src/odbc_driver.cpp:424`, `src/odbc_driver.cpp:492`, `src/odbc_driver.cpp:600`, `src/odbc_handles.cpp:3709`, `src/odbc_handles.cpp:3750`, `src/odbc_handles.cpp:3836`, `src/odbc_handles.cpp:6036`, `tests/test_odbc_driver_integration.cpp:182`, `tests/test_odbc_driver_integration.cpp:216`, `tests/test_odbc_bulk_operations.cpp:76`, `tests/test_odbc_catalog_and_types.cpp:589` |
| `META` | `JDBCBL-META` (catalog/metadata retrieval) | `Partial` | `include/scratchbird/odbc/metadata_helpers.h:147`, `include/scratchbird/odbc/metadata_helpers.h:181`, `include/scratchbird/odbc/metadata_helpers.h:293`, `src/odbc_handles.cpp:2107`, `src/odbc_handles.cpp:2130`, `src/odbc_handles.cpp:2162`, `src/odbc_handles.cpp:510`, `tests/test_odbc_capabilities_browse.cpp:232`, `tests/test_odbc_capabilities_browse.cpp:362`, `tests/test_odbc_capabilities_browse.cpp:403`, `tests/test_odbc_capabilities_browse.cpp:519` (metadata-only recursive schema shaping is implemented for browse metadata paths: database -> default branch row shaping, dotted parent expansion, per-parent uniqueness, and same-name leaves under different parents; broader full-family metadata parity and richer catalog surfaces remain incomplete.) |
| `TYPE` | `JDBCBL-TYPE` (type info and value conversion) | `Implemented` | `src/odbc_driver.cpp:410`, `src/odbc_handles.cpp:2931`, `src/odbc_handles.cpp:4667`, `tests/test_odbc_type_info.cpp:13`, `tests/test_odbc_type_info.cpp:34`, `tests/test_odbc_catalog_and_types.cpp:342` |
| `ERR` | `JDBCBL-ERR` (diagnostics/state mapping) | `Implemented` | `src/odbc_driver.cpp:916`, `src/odbc_driver.cpp:983`, `src/odbc_driver.cpp:1524`, `src/odbc_handles.cpp:287`, `tests/test_odbc_unicode_compat.cpp:103`, `tests/test_odbc_unicode_compat.cpp:162` |
| `RES` | `JDBCBL-RES` (handle/statement/resource lifecycle) | `Implemented` | `src/odbc_driver.cpp:128`, `src/odbc_driver.cpp:180`, `src/odbc_driver.cpp:473`, `src/odbc_handles.cpp:2124`, `src/odbc_handles.cpp:2315` (`OdbcConnection::disconnect`), `src/odbc_handles.cpp:3750`, `tests/test_odbc_catalog_and_types.cpp` (`OdbcLifecycleTest.DisconnectClearsAbandonedSessionState`), `tests/test_odbc_driver_integration.cpp:228`, `tests/test_odbc_external_runtime.cpp` (`ConnectsThroughListenerAndQueriesFixtureData`, `RollbackLeavesImmediateQueryUsableOnFreshBoundary`) |
