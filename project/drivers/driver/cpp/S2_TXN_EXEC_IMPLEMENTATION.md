# S2 TXN + EXEC Implementation (DLB-CPP-003)

## Scope

- Lane: `lanes/active/drivers/cpp`
- Focus: transaction + execution parity improvements with minimal, lane-local changes

## Code Changes

### TXN

- `src/network_client.cpp`
  - Extended `mapProtocolError` to map transaction SQLSTATEs:
    - `25P01` -> `NO_ACTIVE_TRANSACTION`
    - `25P02` -> `TRANSACTION_ABORTED`
    - `25006` -> `READ_ONLY_TRANSACTION`
    - `25*`, `2D000`, `0B000` -> `INVALID_TRANSACTION_STATE`
  - Added `parseReadyAndTrackTransaction(...)` helper and used it on `Ready` handling paths so transaction state is updated consistently.
  - Updated `drainUntilReady` to parse `Ready` payload and track transaction status before returning.

### EXEC

- `src/network_client.cpp`
  - Cleared `last_query_sequence_` on terminal execution outcomes (`Ready` and `Error`) in:
    - `executeQuery`
    - `executePrepared`
    - `executeServerStatement`
    - `executeSblr`
    - `drainUntilReady`
  - This prevents stale post-completion `sendQueryCancel` calls from being treated as valid in-flight cancel attempts.

## Added Tests

- `tests/test_driver_connectivity.cpp`
  - `DriverTxnExecParityTest.TransactionRoundTripBeginCommitRollback`
  - `DriverTxnExecParityTest.RollbackMapsNoActiveTransactionSqlState`
  - `DriverTxnExecParityTest.QueryClearsCancelSequenceAfterReady`
  - `DriverTxnExecParityTest.PrepareAndExecutePreparedRoundTrip`

These tests use a scripted local harness and validate message flow + status behavior for targeted TXN/EXEC paths.

## Test Commands and Results

1. Build targeted test binary
   - Command:
     - `cmake --build lanes/active/drivers/cpp/build_odbc_gate --target scratchbird_client_tests -j4`
   - Result: `PASS`

2. Run new TXN/EXEC parity tests only
   - Command:
     - `lanes/active/drivers/cpp/build_odbc_gate/scratchbird_client_tests --gtest_filter='DriverTxnExecParityTest.*'`
   - Result: `PASS` (`4` tests)

3. Sanity-check existing connectivity suite (same harness file)
   - Command:
     - `lanes/active/drivers/cpp/build_odbc_gate/scratchbird_client_tests --gtest_filter='DriverConnectivitySmokeTest.*'`
   - Result: `PASS` (`5` tests)

## Status Recommendation

- TXN: `PARTIAL`
  - Begin/commit/rollback behavior and key TXN SQLSTATE mapping are now covered.
  - Savepoint parity and broader transaction SQLSTATE/API coverage are still open.

- EXEC: `PARTIAL`
  - Direct query + prepared execution and post-ready cancel-sequence behavior are now covered.
  - In-flight cancel behavior, SBLR execution coverage, server statement APIs, and attach execution coverage remain open.

## Remaining Gaps

- Savepoint behavior parity tests (`SAVEPOINT`, `ROLLBACK TO`, `RELEASE`).
- Additional transaction SQLSTATE mapping tests at API boundary.
- In-flight query cancel test coverage (true active cancel path).
- SBLR execution and server statement (`prepareServerStatement`/`executeServerStatement`/`closeServerStatement`) harness coverage.
- Attach create/detach/list execution coverage.
