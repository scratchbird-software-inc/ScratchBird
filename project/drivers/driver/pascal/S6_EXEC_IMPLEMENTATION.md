# DLB-PASCAL-007 S6 EXEC Implementation

Date: 2026-03-04
Lane: `lanes/active/drivers/pascal`
Scope: close `EXEC` evidence gaps with deterministic lane-local tests (adapter `Prepare` lifecycle, stream-control/backpressure behavior, and first-class batch + multi-result API coverage) plus expanded env-gated live integration assertions.

## Changes Implemented

1. Adapter execution hooks added for deterministic testing and overrideability
   - Files:
     - `src/ScratchBird.FireDAC.pas`
     - `src/ScratchBird.IBX.pas`
     - `src/ScratchBird.Zeos.pas`
     - `src/ScratchBird.SQLdb.pas`
   - Added overridable methods on adapter connection/database classes:
     - `ExecSQLParams(const Sql; const Params)`
     - `ExecuteQueryParams(const Sql; const Params)`
   - Updated adapter query execution paths to route through these hooks instead of calling `Client` directly.
   - Result: adapter query `Prepare` + `ExecSQL` behavior can be asserted without live network dependencies.

2. New deterministic adapter prepare lifecycle suite
   - File: `tests/AdapterPrepareLifecycleTests.pas`
   - Covers:
     - prepare guardrails when connection/database is missing,
     - normalized SQL + parameter ordering reuse after `Prepare`,
     - prepared snapshot behavior (post-prepare SQL/param mutation does not alter prepared execution payload) for:
       - FireDAC adapter
       - IBX adapter
       - Zeos adapter
       - SQLdb adapter

3. Deterministic stream-control/backpressure suite
   - File: `tests/StreamControlBackpressureTests.pas`
   - Added constructor-based transport injection support to client for deterministic wire-flow assertions:
     - `src/ScratchBird.Client.pas`:
       - `constructor CreateWithTransport(const Transport: IScratchBirdTransport)`
       - `procedure InitializeClient(const Transport: IScratchBirdTransport)`
   - Covers:
     - `StreamControl` message emission and payload encoding (`MSG_STREAM_CONTROL`, control/window/timeout).
     - `TScratchBirdResultStream.ReadRow` portal-suspended path emitting `MSG_EXECUTE` resume (`BuildExecutePayload('', CurrentMaxRows)`).
     - command completion metadata (`CommandTag`, `RowsAffected`) through the resume path.
     - generated-key metadata capture (`LastInsertId`, `HasLastInsertId`) from `MSG_COMMAND_COMPLETE` payload `LastId`.
     - async `MSG_NOTICE` handling in result streams via `TScratchBirdClient.HandleAsyncMessage`, preventing unsupported-message failures during stream/read loops.

4. First-class batch execution API with deterministic coverage
   - Files:
     - `src/ScratchBird.Client.pas`
     - `tests/BatchExecutionTests.pas`
   - Added:
     - `TScratchBirdBatchResult` / `TScratchBirdBatchResults` summary model.
     - `TScratchBirdClient.ExecuteBatch(const Statements: array of string): TScratchBirdBatchResults`.
   - Behavior:
     - executes each statement through lane query execution path and returns per-statement summaries:
       - `RowsAffected`
       - `CommandTag`
       - `LastInsertId`
       - `HasLastInsertId`
     - preserves existing SQL normalization/blank-SQL guard behavior.
   - Deterministic tests validate:
     - per-statement summary materialization,
     - generated-key propagation into batch results,
     - emitted wire query payloads for each batch entry.

5. First-class multi-result traversal API with deterministic coverage
   - Files:
     - `src/ScratchBird.Client.pas`
     - `tests/QueryMultiTests.pas`
   - Added:
     - `TScratchBirdRowset` / `TScratchBirdRowsets` result model.
     - `TScratchBirdClient.QueryMulti(const Statements: array of string): TScratchBirdRowsets`.
   - Behavior:
     - executes each statement and materializes per-statement rowset payload:
       - `Columns`
       - `Rows`
       - `RowsAffected`
       - `CommandTag`
       - `LastInsertId`
       - `HasLastInsertId`
     - preserves existing SQL normalization/blank-SQL guard behavior.
   - Deterministic tests validate:
     - row/column materialization for query result sets,
     - command metadata and generated-key propagation for DML result sets,
     - emitted wire query payloads per statement.

6. Expanded env-gated live execution integration coverage
   - File:
     - `tests/IntegrationTest.pas`
     - `docs/fixtures/core_fixture.sql`
     - `scripts/driver_runtime_stack.sh`
   - Added:
     - live stream-control assertion path (`StreamControl(STREAM_RESUME, ...)`) during active query execution.
     - fixture-backed generated-key assertion path (default `generated_key_fixture` insert) with optional overrides:
       - `SCRATCHBIRD_PASCAL_GENERATED_KEY_SQL`
       - `SCRATCHBIRD_PASCAL_GENERATED_KEY_EXPECTED`
     - optional custom stream query control via:
       - `SCRATCHBIRD_PASCAL_STREAM_SQL`
   - Result:
     - live execution integration now covers prepared query, batch, multi-result, stream-control, and fixture-backed generated-key paths in one env-gated suite.

## Targeted Tests Run

1. Adapter lifecycle suite
   - `mkdir -p /tmp/sb_pascal_exec_build /tmp/sb_pascal_exec_bin`
   - `fpc -Mdelphi -Fu./lanes/active/drivers/pascal/src -FU/tmp/sb_pascal_exec_build -FE/tmp/sb_pascal_exec_bin ./lanes/active/drivers/pascal/tests/AdapterPrepareLifecycleTests.pas`
   - `/tmp/sb_pascal_exec_bin/AdapterPrepareLifecycleTests`
   - Result: PASS (`AdapterPrepareLifecycleTests: OK`)

2. Regression checks (core lane suites)
   - `fpc -Mdelphi -Fu./lanes/active/drivers/pascal/src -FU/tmp/sb_pascal_exec_reg_build -FE/tmp/sb_pascal_exec_reg_bin ./lanes/active/drivers/pascal/tests/TxnExecParityTests.pas`
   - `/tmp/sb_pascal_exec_reg_bin/TxnExecParityTests`
   - `fpc -Mdelphi -Fu./lanes/active/drivers/pascal/src -FU/tmp/sb_pascal_exec_reg_build -FE/tmp/sb_pascal_exec_reg_bin ./lanes/active/drivers/pascal/tests/SqlTests.pas`
   - `/tmp/sb_pascal_exec_reg_bin/SqlTests`
  - Result: PASS (`TxnExecParityTests: OK`, `SqlTests: OK`)

3. Stream-control/backpressure suite
   - `fpc -Mdelphi -Fu./lanes/active/drivers/pascal/src -FU/tmp/sb_pascal_exec_stream_build -FE/tmp/sb_pascal_exec_stream_bin ./lanes/active/drivers/pascal/tests/StreamControlBackpressureTests.pas`
   - `/tmp/sb_pascal_exec_stream_bin/StreamControlBackpressureTests`
   - Result: PASS (`StreamControlBackpressureTests: OK`)

4. Batch execution suite
   - `fpc -Mdelphi -Fu./lanes/active/drivers/pascal/src -FU/tmp/sb_pascal_batch_build -FE/tmp/sb_pascal_batch_bin ./lanes/active/drivers/pascal/tests/BatchExecutionTests.pas`
   - `/tmp/sb_pascal_batch_bin/BatchExecutionTests`
   - Result: PASS (`BatchExecutionTests: OK`)

5. Multi-result suite
   - `fpc -Mdelphi -Fu./lanes/active/drivers/pascal/src -FU/tmp/sb_pascal_multi_build -FE/tmp/sb_pascal_multi_bin ./lanes/active/drivers/pascal/tests/QueryMultiTests.pas`
   - `/tmp/sb_pascal_multi_bin/QueryMultiTests`
   - Result: PASS (`QueryMultiTests: OK`)

6. Live integration suite (env-gated compile/run path)
   - `fpc -Mdelphi -Fu./lanes/active/drivers/pascal/src -FU/tmp/sb_pascal_next -FE/tmp/sb_pascal_next ./lanes/active/drivers/pascal/tests/IntegrationTest.pas`
   - `/tmp/sb_pascal_next/IntegrationTest`
   - Result: PASS (`IntegrationTest: SKIPPED (SCRATCHBIRD_PASCAL_URL not set)` in non-env-gated local run)

## EXEC Status Recommendation

- Recommendation: keep `PARTIAL`
- Rationale:
  - adapter prepare lifecycle behavior now has explicit deterministic lane-local assertions.
  - stream-control/backpressure wire behavior now has deterministic lane-local assertions.
  - generated-key retrieval now has first-class result-stream exposure with deterministic tests.
  - first-class batch execution now has deterministic lane-local API coverage.
  - first-class multi-result traversal now has deterministic lane-local API coverage.
  - env-gated live integration now exercises batch/multi-result plus stream-control command paths, with fixture-backed generated-key assertions.
  - status remains partial because advanced live assertions are env-gated/skippable.

## Remaining Gaps

1. Add non-skippable gate execution for advanced live execution assertions (batch/multi-result/stream-control/generated-key).
2. Expand live stream-control assertions from command acceptance to explicit suspended/resume behavior against running fixtures.
