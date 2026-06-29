# JDBC Baseline Requirement Mapping

Baseline mapping of JDBCBL groups to the current JDBC lane implementation status.

## MGA Recovery Contract

- This lane follows ScratchBird's MGA/state-based engine recovery model.
- Reconnect or reopen only repairs transport and session state.
- Reconnect never resurrects abandoned in-flight transactions or replay lost statements.
- Transaction recovery in the lane means reset, rollback, reopen, or retry against engine truth.
- Result resume is valid only for explicit suspended protocol states.
- `SBConnection.prepareTransaction(...)`, `commitPrepared(...)`, and
  `rollbackPrepared(...)` now expose explicit prepared / limbo control
  surfaces through canonical transaction-control SQL.
- `SBConnection.supportsDormantReattach() -> false`,
  `detachToDormant()`, and `reattachDormant(...)` make dormant truth explicit
  and fail closed with `0A000` until the public front door exposes a real
  dormant token flow.
- `setTransactionIsolation(...)` makes the current alias mapping explicit:
  `READ COMMITTED` => canonical `READ COMMITTED`,
  `REPEATABLE READ` / `SNAPSHOT` => canonical `SNAPSHOT`,
  `SERIALIZABLE` => canonical `SNAPSHOT TABLE STABILITY`.
- `TRANSACTION_READ_UNCOMMITTED` is retained only as a legacy compatibility
  alias and does not create a distinct canonical MGA mode.
- `SBConnection.setReadCommittedMode(...)` and
  `SBConnection.setTransactionIsolation(int, int)` expose the canonical
  `READ COMMITTED` sub-mode selector directly for the driver-owned connection
  surface, including `READ COMMITTED READ CONSISTENCY`.
- `SBConnection.canonicalReadCommittedModeLabel(...)` keeps that selector
  source-visible for auditors and lane tests.
- Native `READY`, `TXN_STATUS`, and `current_txn_id` are authoritative for
  transaction activity in this lane; ScratchBird sessions stay always in a
  transaction and `COMMIT` / `ROLLBACK` reopen the next boundary.
- The focused live recovery closure test
  `SBJdbcClosureParityTest.manualTransactionsRemainImmediatelyUsableAcrossCommitAndRollback()`
  now proves that savepoints, commit, rollback, and the first post-rollback
  query remain immediately usable on the live native lane.
- `SBProtocolHandler.retryScopeForSqlState(...)` makes the retry boundary
  explicit: `40001`/`40P01` => statement only, `08xxx` => reconnect only, all
  other SQLSTATEs => no automatic replay.
- `SBProtocolHandler.resumeSuspendedPortal(...)` now rejects unsuspended
  resume with `55000`, and the streaming cursor only arms it after
  `MSG_PORTAL_SUSPENDED`.
- See `../../../../public_audit_summary`.

| JDBCBL group | Current status | Evidence anchors |
| --- | --- | --- |
| `CONN` | Implemented (core and modern JDBC connection APIs, server-resolved schema defaults, public staged auth/bootstrap probe surfaces, resolved-auth reporting, direct `SCRAM_SHA_512`/`TOKEN` execution, and fail-closed admitted-method handling for `MD5`/`PEER`/`REATTACH`) | `src/main/java/com/scratchbird/jdbc/SBDriver.java`; `src/main/java/com/scratchbird/jdbc/SBConnection.java`; `src/main/java/com/scratchbird/jdbc/SBProtocolHandler.java`; `src/main/java/com/scratchbird/jdbc/SBScramClient.java`; `src/main/java/com/scratchbird/jdbc/SBAuthProbeResult.java`; `src/main/java/com/scratchbird/jdbc/SBResolvedAuthContext.java`; `src/test/java/com/scratchbird/jdbc/SBDriverTest.java`; `src/test/java/com/scratchbird/jdbc/SBConnectionModernApiTest.java`; `src/test/java/com/scratchbird/jdbc/SBAuthBootstrapContractTest.java`; `src/test/java/com/scratchbird/jdbc/SBJdbcClosureParityTest.java` |
| `TXN` | Implemented (auto-commit, commit/rollback, savepoints, always-in-transaction semantics, explicit JDBC isolation alias mapping to canonical MGA modes, direct `READ COMMITTED` sub-mode selection on `SBConnection`, explicit prepared / limbo helpers, and native `READY` / `TXN_STATUS` / `current_txn_id` transaction-state handling for ScratchBird's reopened session boundaries) | `src/main/java/com/scratchbird/jdbc/SBConnection.java` (`setAutoCommit`, `commit`, `rollback`, savepoints, `setTransactionIsolation`, `setReadCommittedMode`, `prepareTransaction`, `commitPrepared`, `rollbackPrepared`); `src/main/java/com/scratchbird/jdbc/SBProtocolHandler.java` (`beginTransaction`, `canonicalReadCommittedModeLabel`, `hasActiveTransaction`, native runtime transaction-state handling); `src/main/java/com/scratchbird/jdbc/SBDatabaseMetaData.java` (`supportsSavepoints`); `src/test/java/com/scratchbird/jdbc/SBConnectionTransactionModeTest.java`; `src/test/java/com/scratchbird/jdbc/SBProtocolHandlerSqlStateMappingTest.java`; `src/test/java/com/scratchbird/jdbc/SBJdbcClosureParityTest.java` |
| `EXEC` | Implemented with writeability guards for non-updatable projections | `src/main/java/com/scratchbird/jdbc/SBStatement.java:114-255`; `src/main/java/com/scratchbird/jdbc/SBStatement.java:491-610`; `src/main/java/com/scratchbird/jdbc/SBPreparedStatement.java:430-517`; `src/main/java/com/scratchbird/jdbc/SBCallableStatement.java:107-229`; `src/main/java/com/scratchbird/jdbc/SBResultSet.java:3817-3953`; `src/test/java/com/scratchbird/jdbc/SBStatementMultipleResultsTest.java:29-56`; `src/test/java/com/scratchbird/jdbc/SBStatementGeneratedKeysTest.java:32-90`; `src/test/java/com/scratchbird/jdbc/SBStatementPositionedMutationTest.java:33-99` |
| `META` | Implemented (DatabaseMetaData and ResultSetMetaData surface, anchored to resolved current schema) | `src/main/java/com/scratchbird/jdbc/SBDatabaseMetaData.java`; `src/main/java/com/scratchbird/jdbc/SBResultSetMetaData.java`; `src/test/java/com/scratchbird/jdbc/SBDatabaseMetaDataCapabilitiesTest.java`; `src/test/java/com/scratchbird/jdbc/SBDatabaseMetaDataColumnsTest.java`; `src/test/java/com/scratchbird/jdbc/SBResultSetMetaDataTest.java`; `src/test/java/com/scratchbird/jdbc/SBJdbcClosureParityTest.java` |
| `TYPE` | Implemented (broad JDBC/native type encode/decode and typed retrieval) | `src/main/java/com/scratchbird/jdbc/SBTypeCodec.java:47-217`; `src/main/java/com/scratchbird/jdbc/SBTypeCodec.java:692-760`; `src/main/java/com/scratchbird/jdbc/SBPreparedStatement.java:1334-1388`; `src/main/java/com/scratchbird/jdbc/SBResultSet.java:1930-2017`; `src/test/java/com/scratchbird/jdbc/SBTypeCodecParameterCoverageTest.java:27-157`; `src/test/java/com/scratchbird/jdbc/SBTypedGetObjectConversionTest.java:33-95`; `src/test/java/com/scratchbird/jdbc/SBResultSetArrayTest.java:45-137`; `src/test/java/com/scratchbird/jdbc/SBSQLXMLTest.java:652-717` |
| `ERR` | Implemented (full SQLSTATE mapping, explicit retry-boundary helpers, and SQLSTATE-coded validation errors) | `src/main/java/com/scratchbird/jdbc/SBProtocolHandler.java`; `src/main/java/com/scratchbird/jdbc/SBConnection.java`; `src/main/java/com/scratchbird/jdbc/SBStatement.java`; `src/test/java/com/scratchbird/jdbc/SBProtocolHandlerSqlStateMappingTest.java` |
| `RES` | Implemented (resource lifecycle, pooling, cancellation, resilience/recovery, pool-state reset, dormant fail-closed surfaces, and suspended-only portal resume) | `src/main/java/com/scratchbird/jdbc/SBConnection.java`; `src/main/java/com/scratchbird/jdbc/SBConnectionPool.java`; `src/main/java/com/scratchbird/jdbc/SBProtocolHandler.java`; `src/test/java/com/scratchbird/jdbc/SBConnectionResilienceTest.java`; `src/test/java/com/scratchbird/jdbc/JDBC203PoolingAndRecoveryContractTest.java`; `src/test/java/com/scratchbird/jdbc/SBNativeSQLParityTest.java`; `src/test/java/com/scratchbird/jdbc/SBConnectionTransactionModeTest.java`; `src/test/java/com/scratchbird/jdbc/SBProtocolHandlerSqlStateMappingTest.java` |
