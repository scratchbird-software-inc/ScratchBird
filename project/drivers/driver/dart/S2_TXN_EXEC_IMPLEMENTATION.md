# DLB-DART-003 S2 TXN/EXEC Implementation

## Scope

- Lane: `lanes/active/drivers/dart`
- Focus: transaction + execution parity hardening with minimal lane-local changes

## Changes Applied

1. TXN guardrails and state tracking (`lib/src/client.dart`)
- Added transaction state guards:
  - `begin` now rejects when a transaction is already active.
  - `commit`, `rollback`, `savepoint`, `releaseSavepoint`, and `rollbackToSavepoint` now require an active transaction.
  - Savepoint operations now reject empty/blank savepoint names.
- Added explicit `txnStatus` async handling so incoming transaction status frames update local `_txnId`.
- Added resilient txn-id extraction helper for ready/status payloads with header fallback.

2. EXEC safety hardening (`lib/src/client.dart`)
- Added SQL text validation: `query` now rejects empty SQL early.
- Hardened cancel behavior:
  - `_lastQuerySequence` is tracked as nullable.
  - `cancel` now rejects when no active in-flight query sequence exists.
- Cleared/rebased cancel sequence tracking on terminal and paging paths:
  - Clear on query completion (`ready`) and query error.
  - Refresh sequence when resuming execution from `portalSuspended`.
  - Clear on `close`/`terminate`.

3. Focused TXN/EXEC tests (`test/txn_exec_parity_test.dart`)
- Added deterministic lane-local tests for:
  - TXN guardrails (`commit`/`rollback`/`savepoint` require active transaction).
  - TXN payload encoding (`begin`, `savepoint`, `release`, `rollback-to`).
  - EXEC guardrails (empty SQL rejection, cancel-without-inflight rejection).
  - EXEC payload encoding (`query`, `execute`, `cancel`).

4. Baseline mapping refresh
- Updated TXN/EXEC rows in `BASELINE_REQUIREMENT_MAPPING.md` with new source anchors, test anchors, and gap statements.

## Targeted Tests Run

1. `dart test test/txn_exec_parity_test.dart`
- Result: `PASS` (`10` tests passed)

## Final Status Recommendation

- TXN: `PARTIAL`
- EXEC: `PARTIAL`

## Rationale

- TXN parity improved with explicit invalid-operation guardrails, savepoint input validation, and async txn-status synchronization, but lacks live integration validation for transaction semantics and failure handling.
- EXEC parity improved with deterministic guardrails and cancel-sequence lifecycle hardening, but still lacks live execution-path coverage for server-driven async/result behavior.

## Remaining Gaps

- Add live integration tests for begin/commit/rollback/savepoint success and error paths.
- Add live execution integration tests for simple query, parameterized query, paging (`portalSuspended`), and SBLR execution.
- Add targeted async execution tests validating `queryPlan`, `notification`, and `sblrCompiled` handling under real wire flow.
