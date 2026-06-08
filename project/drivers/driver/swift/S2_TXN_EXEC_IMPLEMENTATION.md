# DLB-SWIFT-003 S2 TXN + EXEC Implementation (Swift Lane)

## Scope

- Lane: `lanes/active/drivers/swift`
- Focus: transaction + execution parity hardening with minimal lane-local changes

## Changes Applied

1. TXN safety and option validation (`Sources/ScratchBird/TxnExecValidation.swift`, `Sources/ScratchBird/Connection.swift`)
- Added `validateTxnBeginOptions(...)` and invoked it from `begin(...)` so unsupported `isolationLevel`, `accessMode`, and `autocommitMode` values fail fast before wire send.
- Added `normalizeSavepointName(...)` and applied it to `savepoint`, `releaseSavepoint`, and `rollbackToSavepoint` to reject blank/whitespace-only names.

2. EXEC cancel/resume guardrails (`Sources/ScratchBird/TxnExecValidation.swift`, `Sources/ScratchBird/Connection.swift`)
- Added `requireCancelableSequence(...)` and used it in `cancel()` to reject cancel requests when there is no tracked active query sequence.
- Added `normalizePortalResumeMaxRows(fetchSize:)` and used it for `.portalSuspended` resume execution to clamp invalid/negative `fetchSize` values safely into `UInt32`.
- Updated result collection to clear `lastQuerySequence` on terminal outcomes (`.ready`, `.error`) so post-completion cancel does not target stale sequence ids.
- Updated `.portalSuspended` resume path to track the fresh execute sequence id.

3. Targeted TXN/EXEC lane tests (`Tests/ScratchBirdTests/TxnExecParityTests.swift`)
- Added focused tests for:
  - TXN begin option validation (accept/reject paths).
  - Savepoint name normalization and blank-name rejection.
  - TXN wire payload construction (`begin`, `commit`, `rollback`, `savepoint`).
  - EXEC helpers (`requireCancelableSequence`, `normalizePortalResumeMaxRows`).
  - EXEC wire payload construction (`execute`, `cancel`).

4. Baseline mapping refresh
- Updated TXN and EXEC sections in `BASELINE_REQUIREMENT_MAPPING.md` with new source/test anchors and refreshed gap statements.

## Targeted Tests Run

1. `swift test --filter TxnExecParityTests`
- Result: `PASS`
- Executed: `10` tests, `0` failures

## TXN Status

- Recommendation: `PARTIAL`
- Why:
  - Begin/savepoint input guardrails and TXN payload coverage are now present with deterministic lane tests.
  - Remaining parity risk is runtime/behavioral: no live integration evidence yet for full server-side TXN lifecycle semantics and failure-path state handling.

## EXEC Status

- Recommendation: `PARTIAL`
- Why:
  - Cancel/resume sequence tracking and fetch-size safety are improved, and key EXEC payload/helper behavior is now unit-tested.
  - Remaining parity risk is end-to-end behavior: no live integration evidence yet for simple/parameterized execution plus cancellation timing and portal resume semantics.

## Remaining Gaps

- Add wire/integration TXN tests covering begin/commit/rollback/savepoint under success + server error conditions.
- Add wire/integration EXEC tests for simple and parameterized query paths, cancellation timing, and portal suspend/resume behavior.
- Expand EXEC parity surface coverage for batch/multi-result/generated-key style behavior if required by lane scope.
