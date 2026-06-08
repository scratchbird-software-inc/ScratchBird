# S2 TXN/EXEC Implementation (DLB-DOTNET-003)

Scope: `lanes/active/drivers/dotnet` lane only.

## Changes

- Added callable SQL normalization to `SqlHelpers`:
  - `NormalizeCallable(...)`
  - `NormalizeCallableSql(...)`
  - JDBC escape-call support (`{ call ... }`, `{ ? = call ... }`)
- Added EXEC parity result models:
  - `FieldSummary`
  - `ResultSetSummary`
  - `BatchItemSummary`
  - `BatchSummary`
- Added protocol-level multi-result collector in `ProtocolClient`:
  - `ExecuteQueryMulti(...)`
  - captures per-command rows, fields, command tag, and `LastInsertId`.
- Added high-level EXEC parity APIs on `ScratchBirdConnection`:
  - `NativeSql(...)`
  - `NativeCallableSql(...)`
  - `Call(...)`
  - `QueryMulti(...)` / `ExecuteMulti(...)`
  - `ExecuteBatch(...)` / `QueryBatch(...)`
  - `ExecuteWithGeneratedKeys(...)`
- Added safe parameterless multi-statement splitter fallback in `QueryMulti(...)` so independent result-set traversal remains available even when runtime multi-statement framing behavior varies.
- Preserved and retained previously added TXN guardrails:
  - single active transaction ownership
  - savepoint lifecycle validation
  - isolation mapping updates (`Snapshot`/`Chaos` -> serializable wire level).

## Tests Run

- `dotnet test tests/ScratchBird.Data.Tests/ScratchBird.Data.Tests.csproj --filter "FullyQualifiedName~SqlHelpersTests|FullyQualifiedName~TransactionExecutionParityTests"`: **PASS** (17 passed, 0 failed)
- `dotnet test tests/ScratchBird.Data.Tests/ScratchBird.Data.Tests.csproj --filter "FullyQualifiedName~IntegrationTests.QueryMultiReturnsIndependentResultSets|FullyQualifiedName~IntegrationTests.ExecuteBatchReturnsSummary|FullyQualifiedName~IntegrationTests.CallableEscapeSyntaxExecutes|FullyQualifiedName~IntegrationTests.ExecuteWithGeneratedKeysReturnsKeyCollection"`: **PASS** (4 passed, 0 failed)

## TXN Status

- Recommendation: **PARTIAL**
- Reason: transaction lifecycle and active-ownership guardrails are explicit/tested and snapshot/chaos now map deterministically, but full live isolation-matrix validation remains pending.

## EXEC Status

- Recommendation: **IMPLEMENTED**
- Reason: callable normalization/execution, multi-result traversal, batch execution summaries, and generated-key extraction are now exposed and validated with unit + targeted integration coverage.

## Remaining Gaps

- Isolation-level semantics are still bounded to the wire isolation enum (`read-uncommitted`, `read-committed`, `repeatable-read`, `serializable`) and not yet integration-verified across full server behavior matrix.
- Provider-specific output-parameter semantics remain outside the current native baseline scope.
