# DLB-NODE-003 S2 TXN/EXEC Implementation

## Changes
- Added deterministic transaction-state guards in `Client`:
  - `beginTransaction` now rejects nested begin when a transaction is already active.
  - `commitTransaction`, `rollbackTransaction`, `savepoint`, `releaseSavepoint`, and `rollbackToSavepoint` now reject calls when no active transaction exists.
  - Savepoint operations now reject empty/blank savepoint names.
- Added transaction-state synchronization helper (`applyTxnState`) and wired it into READY/current transaction status handling so local guards follow wire-reported transaction id transitions.
- Added `nativeSQL(text, params?)` to provide an explicit native SQL normalization entry point for execution parity.
- Added explicit transaction/session parity API surface:
  - `getAutoCommit()` / `setAutoCommit(enabled)` with deterministic transition behavior.
  - `getSessionSchema()` / `setSessionSchema(schema)` with immediate session apply on connected clients.
  - Implicit transaction begin when `autoCommit=false` and execution starts without an active transaction.
- Added expanded execution API coverage:
  - `queryMulti` and `executeMulti` return distinct result sets.
  - `queryBatch` and `executeBatch` return per-item command summaries.
  - `queryBatch` and `executeBatch` now reject empty batch parameter sets (`07001`).
  - `call` exposes JDBC callable-style invocation normalization and execution.
  - Added `executeWithGeneratedKeys(text, params?, options?)` returning non-zero generated key list (`lastId` values).
- Added normalization error typing parity:
  - Query normalization failures now map to `ScratchbirdSyntaxError` (`07001`) across:
    - `query`, `queryMulti`, `queryStream`,
    - `nativeSQL`, `nativeCallableSQL`,
    - `call`, `execute`, and `executeMulti`.
- Added targeted Node lane unit tests for:
  - Transaction lifecycle and invalid-operation guards.
  - Autocommit transitions and implicit transaction behavior.
  - Session schema setter/getter and emitted schema statement.
  - Savepoint guard behavior and wire call sequence.
  - Extended query wire sequence and named-parameter rewrite.
  - Prepared execute wire sequence and `nativeSQL` normalization.
  - `nativeSQL` / `nativeCallableSQL` normalization failure typing (`07001`).
  - Multi-result and generated-key (`lastId`) result propagation.
  - `executeWithGeneratedKeys` key filtering behavior.
  - Empty-batch rejection for `queryBatch` and `executeBatch`.
  - Callable execution SQL rewrite/delegation.

## Tests Run
- `npm run build && node --test test/unit.test.js` -> PASS
- `npm test` -> PASS
  - Unit suite passes.
  - Integration suite now includes env-gated coverage for:
    - `queryMulti` live result-set separation,
    - prepared `executeMulti`,
    - `queryBatch` and `executeBatch` summary surfaces,
    - callable execution (`call` with JDBC escape syntax),
    - `executeWithGeneratedKeys` key list surface,
    - autocommit toggle with implicit transaction lifecycle,
    - explicit savepoint lifecycle in transaction mode,
    - stream paging/resume behavior (`queryStream` with `maxRows`).

## TXN Status
- Recommendation: `IMPLEMENTED`
- Why:
  - Implemented and tested: explicit begin/commit/rollback, savepoint create/release/rollback-to, deterministic invalid-operation guards, first-class autocommit/session-schema controls, and env-gated live transaction/savepoint assertions.

## EXEC Status
- Recommendation: `IMPLEMENTED`
- Why:
  - Implemented and tested: simple and prepared execution, positional/named bind normalization, streaming, cancellation, `nativeSQL`/`nativeCallableSQL`, batch execution summaries with empty-batch guards, multi-result traversal, generated-key propagation, explicit generated-key list API, callable/routine API, and env-gated live stream pagination coverage.

## Remaining Gaps
- None for baseline `JDBCBL-TXN` / `JDBCBL-EXEC` scope.
