# DLB-RUBY-003 S2 TXN/EXEC Implementation

Date: 2026-03-04
Lane: `lanes/active/drivers/ruby`
Scope: transaction and execution parity improvements with targeted lane-local tests.

## Changes Implemented

1. Added callable SQL normalization and translation surfaces
   - File: `lib/scratchbird/sql.rb`
   - Added:
     - `Sql.normalize_callable(...)`
     - `Sql.normalize_callable_sql(...)`
   - Supports JDBC escape syntax:
     - `{ call routine(...) }`
     - `{ ? = call routine(...) }`

2. Added EXEC parity models for multi/batch summaries
   - File: `lib/scratchbird/result.rb`
   - Added:
     - `FieldSummary`
     - `ResultSetSummary`
     - `BatchItemSummary`
     - `BatchSummary`
   - Extended `Result` with `last_insert_id` capture.

3. Added high-level EXEC parity APIs to `Client`
   - File: `lib/scratchbird/client.rb`
   - Added:
     - `native_sql`
     - `native_callable_sql`
     - `call`
     - `query_multi` / `execute_multi`
     - `execute_batch` / `query_batch`
     - `execute_with_generated_keys`
   - Added internal helpers:
     - `summarize_result`
     - `split_sql_statements`
   - Updated command-complete handling to persist `last_insert_id`.

4. Added high-level EXEC parity APIs to `Connection`
   - File: `lib/scratchbird/connection.rb`
   - Added:
     - `native_sql`
     - `native_callable_sql`
     - `call`
     - `query_multi` / `execute_multi`
     - `execute_batch` / `query_batch`
     - `execute_with_generated_keys`
   - These use existing transaction gate semantics (`begin_transaction_if_needed`) so `autocommit=false` behavior remains consistent across direct, prepared, and new parity APIs.

5. Added/expanded test coverage
   - `test/test_sql.rb`
     - callable normalization behavior
   - `test/test_txn_exec_parity.rb`
     - native SQL forwarding and new EXEC parity surface forwarding/transaction gate behavior
   - `test/test_result_stream.rb`
     - `Result`/`ResultStream` hash iteration, command summary (`rowcount`, `command_tag`, `last_insert_id`), and single-consumption guard
   - `test/test_integration.rb` (env-gated)
     - `query_multi`
     - `execute_batch`
     - callable escape execution
     - generated-keys collection

## Targeted Tests Run

1. `ruby -Ilib:test test/test_sql.rb`
   - Result: PASS
   - Output summary: `5 runs, 9 assertions, 0 failures, 0 errors, 0 skips`

2. `ruby -Ilib:test test/test_txn_exec_parity.rb`
   - Result: PASS
   - Output summary: `9 runs, 48 assertions, 0 failures, 0 errors, 0 skips`

3. `ruby -Ilib:test test/test_integration.rb`
   - Result: PASS
   - Output summary: `8 runs, 0 assertions, 0 failures, 0 errors, 8 skips` (env-gated)

4. `ruby -Ilib:test test/test_result_stream.rb`
   - Result: PASS
   - Output summary: `3 runs, 13 assertions, 0 failures, 0 errors, 0 skips`

5. `ruby -Ilib:test -e 'Dir["test/test_*.rb"].sort.each { |f| require_relative f }'`
   - Result: PASS
   - Output summary: `54 runs, 214 assertions, 0 failures, 0 errors, 8 skips`

## Status Recommendation

- TXN: `PARTIAL`
- EXEC: `IMPLEMENTED`

Rationale:
- TXN guardrails and savepoint lifecycle remain present and tested.
- EXEC now includes callable normalization/execution, multi-result traversal, batch summaries, and generated-key extraction with deterministic unit coverage and env-gated integration coverage.

## Remaining Gaps

1. TXN:
   - No live-wire assertions of `READY` transaction state transitions under real server behavior.
   - No deterministic coverage for commit/rollback error handling after server-side transaction aborts.

2. EXEC:
   - Deeper live-wire coverage for portal suspend/resume and true single-request multi-result flows is still pending.
   - Environment-backed integration is currently gated and may skip when runtime DSN is not provided.
