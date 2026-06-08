# S2 TXN/EXEC Implementation (DLB-PASCAL-003)

Scope: `lanes/active/drivers/pascal` only.

## What Changed

- Added lane-local transaction guardrails in `src/ScratchBird.Client.pas`:
  - Added `EnsureConnected` (`08003`) and applied it to transaction begin and execution entry points.
  - `BeginTransactionEx` now rejects nested begin when `FTxnId <> 0` (`25000`, `transaction already active`).
  - `Commit`/`Rollback` now no-op when no active transaction (`FTxnId = 0`) to avoid unnecessary wire calls.
  - `Savepoint`/`ReleaseSavepoint`/`RollbackToSavepoint` now:
    - require non-blank savepoint names (`42601`, `savepoint name is required`),
    - require an active transaction (`25000`, `<op> requires an active transaction`).
  - `Disconnect` now clears local transaction/attachment state (`FTxnId := 0`, zero attachment bytes).
- Added execution input validation in `src/ScratchBird.Client.pas`:
  - Added `NormalizeSqlText`; `ExecSQLParams` and `ExecuteQueryParams` reject blank SQL early (`42601`, `SQL text is required`).
- Fixed execution SQL normalization parity in `src/ScratchBird.Sql.pas`:
  - `NormalizeNamedSql` now preserves PostgreSQL-style cast markers (`::`) and only treats `@name`/`:name` forms with identifier starts as named parameters.
- Added/updated lane tests:
  - New `tests/TxnExecParityTests.pas` for TXN guardrails and EXEC validation/normalization checks.
  - Updated `tests/SqlTests.pas` to current normalization APIs and added cast-syntax normalization coverage.
- Implemented adapter `Prepare` behavior in:
  - `src/ScratchBird.FireDAC.pas`
  - `src/ScratchBird.IBX.pas`
  - `src/ScratchBird.SQLdb.pas`
  - `src/ScratchBird.Zeos.pas`
  so `Prepare` now normalizes/caches SQL and parameter ordering for reuse by `Open`/`ExecSQL`.
- Added adapter advanced transaction forwarding surface (`StartTransactionEx`) in:
  - `src/ScratchBird.FireDAC.pas`
  - `src/ScratchBird.IBX.pas`
  - `src/ScratchBird.SQLdb.pas`
  - `src/ScratchBird.Zeos.pas`
  so adapter callers can access `BeginTransactionEx` options parity without dropping to the raw client API.
- Added `tests/AdapterTransactionOptionsTests.pas` for disconnected guard parity on adapter `StartTransactionEx` across all four adapter surfaces.
- Added deterministic transaction-state transition suite:
  - `tests/TxnStateTransitionsTests.pas`
  - validates wire-`READY` lifecycle transitions for begin/savepoint/release/rollback-to/commit and begin/rollback flows.
  - validates post-commit/post-rollback active-transaction guard behavior via savepoint/release calls.
  - validates `BeginTransactionEx` option-matrix payload encoding for:
    - full option path (`isolation/access/deferrable/wait/timeout/autocommit/conflict`),
    - minimal option path (`isolation` only).
  - validates deterministic conflict-path behavior for `BeginTransactionEx` via injected `MSG_ERROR` (`40001`) with retry + no-active-txn guard assertions after failed begin.
- Expanded env-gated live integration coverage in:
  - `tests/IntegrationTest.pas`
  - validates live begin/savepoint/release/rollback-to/commit and begin/rollback lifecycle execution.
  - validates live batch (`ExecuteBatch`) and multi-result (`QueryMulti`) execution paths.
  - validates live stream-control command path during active query execution.
  - validates fixture-backed generated-key assertions by default, with optional SQL/expected-id overrides via `SCRATCHBIRD_PASCAL_GENERATED_KEY_SQL` and `SCRATCHBIRD_PASCAL_GENERATED_KEY_EXPECTED`.
- Added shared generated-key fixture provisioning:
  - `docs/fixtures/core_fixture.sql` now seeds `generated_key_fixture` (identity-backed key table).
  - `scripts/driver_runtime_stack.sh` fixture reset now drops/reloads `generated_key_fixture`.
- Updated TXN/EXEC evidence and gaps in `BASELINE_REQUIREMENT_MAPPING.md`.

## Targeted Tests Run

1. `fpc -Mdelphi -Fu./lanes/active/drivers/pascal/src -FE./lanes/active/drivers/pascal/tests ./lanes/active/drivers/pascal/tests/TxnExecParityTests.pas`
- Result: PASS (compile succeeded).

2. `./lanes/active/drivers/pascal/tests/TxnExecParityTests`
- Result: PASS (`TxnExecParityTests: OK`).

3. `fpc -Mdelphi -Fu./lanes/active/drivers/pascal/src -FE./lanes/active/drivers/pascal/tests ./lanes/active/drivers/pascal/tests/SqlTests.pas`
- Result: PASS (compile succeeded).

4. `./lanes/active/drivers/pascal/tests/SqlTests`
- Result: PASS (`SqlTests: OK`).

5. `fpc -Mdelphi -Fu./lanes/active/drivers/pascal/src -FU/tmp/sb_pascal_txn_opts_build -FE/tmp/sb_pascal_txn_opts_bin ./lanes/active/drivers/pascal/tests/AdapterTransactionOptionsTests.pas`
- Result: PASS (compile succeeded).

6. `/tmp/sb_pascal_txn_opts_bin/AdapterTransactionOptionsTests`
- Result: PASS (`AdapterTransactionOptionsTests: OK`).

7. `fpc -Mdelphi -Fu./lanes/active/drivers/pascal/src -FU/tmp/sb_pascal_txn_state_build -FE/tmp/sb_pascal_txn_state_bin ./lanes/active/drivers/pascal/tests/TxnStateTransitionsTests.pas`
- Result: PASS (compile succeeded).

8. `/tmp/sb_pascal_txn_state_bin/TxnStateTransitionsTests`
- Result: PASS (`TxnStateTransitionsTests: OK`).

9. `fpc -Mdelphi -Fu./lanes/active/drivers/pascal/src -FU/tmp/sb_pascal_next -FE/tmp/sb_pascal_next ./lanes/active/drivers/pascal/tests/IntegrationTest.pas`
- Result: PASS (compile succeeded).

10. `/tmp/sb_pascal_next/IntegrationTest`
- Result: PASS (`IntegrationTest: SKIPPED (SCRATCHBIRD_PASCAL_URL not set)` in non-env-gated local run).

## TXN Status

- Recommendation: `PARTIAL`

Rationale:
- Deterministic lane-local TXN guardrails now exist and are covered by dedicated tests (disconnected begin, no-active-txn commit/rollback behavior, savepoint active-txn and name validation, txn payload encoding checks).
- Deterministic transaction state transitions are now asserted end-to-end against wire `READY` transaction ids for begin/savepoint/release/rollback-to/commit and begin/rollback paths.
- Deterministic `BeginTransactionEx` option-matrix payload encoding is now asserted directly against emitted wire payloads.
- Deterministic `BeginTransactionEx` conflict-path behavior is now asserted via injected `40001` error frames, including SQLSTATE mapping, emitted begin payload verification, and retry begin/rollback flow after failure.
- Adapter surfaces now expose advanced transaction options via `StartTransactionEx` forwarding, with deterministic lane-local guard tests.
- Env-gated live integration now exercises begin/savepoint/release/rollback-to/commit and begin/rollback lifecycle paths.
- Remaining gaps preventing `MET`: live coverage is env-gated/skippable and does not yet include live conflict-path assertions for `BeginTransactionEx` option matrices against running fixtures.

## EXEC Status

- Recommendation: `PARTIAL`

Rationale:
- Deterministic lane-local execution parity improved with blank SQL validation, cast-safe named-parameter normalization, adapter `Prepare` normalization/cache behavior, stream-control/backpressure assertions, and generated-key metadata exposure.
- Env-gated live integration now exercises prepared query execution plus live batch/multi-result and stream-control paths.
- Env-gated live integration now includes fixture-backed generated-key assertions with optional SQL/expected-id overrides.
- Remaining gaps prevent `MET`: live advanced execution coverage remains env-gated/skippable.

## Remaining Concrete Gaps

- TXN:
  - Add non-skippable gate execution for live transaction lifecycle assertions.
  - Expand live assertions for `BeginTransactionEx` option matrices to include conflict-path behavior against running fixtures.
- EXEC:
  - Add non-skippable gate execution for live advanced execution assertions.
  - Expand live stream-control/backpressure assertions beyond command acceptance to explicit suspended/resume behavior against running server fixtures.
