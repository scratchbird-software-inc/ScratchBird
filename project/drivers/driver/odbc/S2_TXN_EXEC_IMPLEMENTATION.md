# S2 TXN/EXEC Implementation (DLB-ODBC-003)

Scope: `lanes/active/drivers/odbc` lane only.

## Changes

- Implemented environment-level transaction fan-out for `SQLEndTran(SQL_HANDLE_ENV, ...)`.
  - `src/odbc_driver.cpp:890` now delegates ENV handles to `OdbcEnvironment::endTransaction`.
  - `src/odbc_handles.cpp:1774` adds `OdbcEnvironment::endTransaction` to validate completion type and commit/rollback all connected child connections.
- Kept connection-level commit/rollback path as authoritative transaction operation path.
  - `src/odbc_handles.cpp:2389` (`OdbcConnection::endTransaction`).
- Fixed execution parity for warning-success return codes.
  - `src/odbc_handles.cpp:3562` treats `SQL_SUCCESS_WITH_INFO` as successful in `OdbcConnection::executeSQL`.
  - `src/odbc_handles.cpp:6036` propagates aggregate `SQL_SUCCESS_WITH_INFO` from `executeSqlStatements`.
  - `src/odbc_handles.cpp:3750` clears prepared state for `execDirect` on both `SQL_SUCCESS` and `SQL_SUCCESS_WITH_INFO`.
- Added focused lane tests for TXN and EXEC behavior.
  - `tests/test_odbc_catalog_and_types.cpp:551` (`OdbcTransactionTest.EnvHandleEndTranCommitsConnectedConnections`).
  - `tests/test_odbc_catalog_and_types.cpp:589` (`OdbcExecutionParityTest.ExecuteAndExecDirectPropagateSuccessWithInfo`).

## Tests Run

- `cmake --build build --target scratchbird_odbc_tests -j 4` -> PASS
- `./build/lanes/active/drivers/odbc/scratchbird_odbc_tests --gtest_filter='OdbcTransactionTest.EnvHandleEndTranCommitsConnectedConnections:OdbcExecutionParityTest.ExecuteAndExecDirectPropagateSuccessWithInfo'` -> PASS (2/2)
- `./build/lanes/active/drivers/odbc/scratchbird_odbc_tests --gtest_filter='OdbcAutocommitTest.*:OdbcTransactionTest.EnvHandleEndTranCommitsConnectedConnections:OdbcExecutionParityTest.ExecuteAndExecDirectPropagateSuccessWithInfo'` -> PASS (4/4)

## TXN Status

- Recommendation: **MET**
- Basis: autocommit + isolation mapping remain covered, DBC `SQLEndTran` commit/rollback path is present, and ENV-handle `SQLEndTran` now performs real fan-out across connected child connections with lane test coverage.

## EXEC Status

- Recommendation: **MET**
- Basis: direct and prepared execution paths now preserve ODBC warning-success semantics (`SQL_SUCCESS_WITH_INFO`) rather than treating warnings as hard failures, with focused parity tests validating both `execute()` and `execDirect()`.

## Remaining Gaps

- Transaction tests currently validate ENV-handle commit fan-out; a dedicated ENV rollback failure-propagation assertion is still not explicitly covered.
- Execution tests validate warning-success propagation but do not add new multi-result-set/`SQLMoreResults` assertions.
