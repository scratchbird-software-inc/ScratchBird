# Baseline Requirement Mapping (DOTNETBL -> JDBC Baseline)

Scope: `lanes/active/drivers/dotnet` lane only.

Status legend:
- `Implemented`: baseline behavior is present and anchored by lane source/tests.
- `Partial`: baseline behavior exists but has explicit scope limits or incomplete validation coverage.

## MGA Recovery Contract

- This lane follows ScratchBird's MGA/state-based engine recovery model.
- Reconnect or reopen only repairs transport and session state.
- Reconnect never resurrects abandoned in-flight transactions or replay lost statements.
- Transaction recovery in the lane means reset, rollback, reopen, or retry against engine truth.
- Result resume is valid only for explicit suspended protocol states.
- `PrepareTransaction(...)`, `CommitPrepared(...)`, and
  `RollbackPrepared(...)` now expose explicit prepared / limbo control
  surfaces through canonical transaction-control SQL.
- `SupportsDormantReattach() -> true`, `DetachToDormant()`, and
  `ReattachDormant(...)` now expose the explicit dormant token flow on the
  native public lane, with engine-issued `dormant_id` plus
  `dormant_reattach_token` carried through the public startup contract.
- `BeginTransaction(ScratchBirdTransactionOptions)` exposes the canonical MGA
  begin flags for `IsolationLevel`, `AccessMode`, `Deferrable`, `Wait`,
  `TimeoutMs`, `AutoCommit`, and `ReadCommittedMode`.
- Native `READY`, `TXN_STATUS`, and `current_txn_id` are authoritative
  transaction-state surfaces; ScratchBird sessions stay always in a
  transaction and `COMMIT` / `ROLLBACK` immediately reopen the next boundary.
- Begin on the live native lane restarts the current default boundary with
  the requested settings; overlapping caller-owned transaction objects are
  still rejected by the ADO.NET connection wrapper.
- Current isolation alias mapping is explicit in lane source:
  `IsolationLevel.ReadCommitted` => canonical `READ COMMITTED`,
  `IsolationLevel.RepeatableRead` => canonical `SNAPSHOT`,
  `IsolationLevel.Serializable` / `Snapshot` / `Chaos` => canonical
  `SNAPSHOT TABLE STABILITY`.
- `ScratchBirdReadCommittedMode` now exposes the canonical `READ COMMITTED`
  sub-modes directly, including `READ COMMITTED READ CONSISTENCY`.
- `ScratchBirdSqlStateMapper.RetryScopeForSqlState(...)` makes the retry
  boundary explicit: `40001`/`40P01` => statement only, `08xxx` => reconnect
  only, all other SQLSTATEs => no automatic replay.
- `ProtocolClient.ResumeSuspendedPortal(...)` now rejects unsuspended resume
  with `55000`, and the query/read paths only call it after
  `PORTAL_SUSPENDED`.
- See `../../../../public_audit_summary`.

| DOTNETBL group | JDBC baseline group | Current status | Evidence anchors (lane source/tests) | Notes |
|---|---|---|---|---|
| `CONN` | `JDBCBL-CONN` | `Implemented` | `src/ScratchBird.Data/AuthBootstrap.cs`, `src/ScratchBird.Data/Config.cs`, `src/ScratchBird.Data/ProtocolClient.cs`, `src/ScratchBird.Data/ScratchBirdConnection.cs`, `src/ScratchBird.Data/ScratchBirdConnectionStringBuilder.cs`, `tests/ScratchBird.Data.Tests/AuthBootstrapContractTests.cs`, `tests/ScratchBird.Data.Tests/ConfigTests.cs`, `tests/ScratchBird.Data.Tests/ConnectionStringBuilderSurfaceTests.cs`, `tests/ScratchBird.Data.Tests/IntegrationTests.cs:26`, `tests/ScratchBird.Data.Tests/IntegrationTests.cs:424`, `tests/ScratchBird.Data.Tests/IntegrationTests.cs:852`, `tests/ScratchBird.Data.Tests/TransactionExecutionParityTests.cs:39` | Open/close, reconnect on stale client, staged direct/manager auth probe, resolved-auth reporting, `SCRAM_SHA_512`, generic `TOKEN`, and fail-closed negotiated unsupported methods are implemented. The lane now also has deterministic proof that stale-client reconnect clears the local active-transaction claim before reopen failure can leak abandoned transaction state across sessions. Live integration proof still requires a reachable listener through `SCRATCHBIRD_DOTNET_URL` or the lane's default fallback DSN. |
| `TXN` | `JDBCBL-TXN` | `Implemented` | `src/ScratchBird.Data/ScratchBirdConnection.cs:223`, `src/ScratchBird.Data/ScratchBirdConnection.cs:326`, `src/ScratchBird.Data/TransactionOptions.cs:13`, `src/ScratchBird.Data/ProtocolClient.cs:305`, `tests/ScratchBird.Data.Tests/IntegrationTests.cs:447`, `tests/ScratchBird.Data.Tests/IntegrationTests.cs:1219`, `tests/ScratchBird.Data.Tests/TransactionExecutionParityTests.cs:19`, `tests/ScratchBird.Data.Tests/TransactionExecutionParityTests.cs:252` | Commit/rollback/savepoint flows and single-active-transaction enforcement are implemented, begin-transaction parity now includes optioned wire payload support (isolation/read-committed-mode/access/deferrable/wait/timeout/autocommit), native `READY`/`TXN_STATUS`/`current_txn_id` handling for active-with-zero-`txn_id` fresh boundaries, default `READ COMMITTED` fresh-boundary adoption on the live native lane, and explicit fail-closed rejection for non-default fresh-boundary adoption until a live server path exists. The lane also has explicit prepared / limbo control helpers (`PrepareTransaction`, `CommitPrepared`, `RollbackPrepared`) with deterministic helper, wire-shape, and focused live savepoint tests. |
| `EXEC` | `JDBCBL-EXEC` | `Implemented` | `src/ScratchBird.Data/ScratchBirdCommand.cs:56`, `src/ScratchBird.Data/ScratchBirdCommand.cs:129`, `src/ScratchBird.Data/ScratchBirdConnection.cs:272`, `src/ScratchBird.Data/ScratchBirdConnection.cs:278`, `src/ScratchBird.Data/ScratchBirdConnection.cs:284`, `src/ScratchBird.Data/ScratchBirdConnection.cs:311`, `src/ScratchBird.Data/ScratchBirdConnection.cs:356`, `src/ScratchBird.Data/ScratchBirdConnection.cs:419`, `src/ScratchBird.Data/ProtocolClient.cs:180`, `src/ScratchBird.Data/SqlHelpers.cs:37`, `src/ScratchBird.Data/SqlHelpers.cs:43`, `tests/ScratchBird.Data.Tests/SqlHelpersTests.cs:53`, `tests/ScratchBird.Data.Tests/TransactionExecutionParityTests.cs:131`, `tests/ScratchBird.Data.Tests/TransactionExecutionParityTests.cs:144`, `tests/ScratchBird.Data.Tests/IntegrationTests.cs:462`, `tests/ScratchBird.Data.Tests/IntegrationTests.cs:486`, `tests/ScratchBird.Data.Tests/IntegrationTests.cs:515`, `tests/ScratchBird.Data.Tests/IntegrationTests.cs:539` | Core execute/read/scalar/prepare/cancel paths remain in place and lane now includes callable SQL normalization, multi-result traversal, batch summaries, and generated-key extraction. Output-parameter semantics remain outside current baseline scope. |
| `META` | `JDBCBL-META` | `Implemented` | `src/ScratchBird.Data/ScratchBirdConnection.cs:449`, `src/ScratchBird.Data/ScratchBirdConnection.cs:881`, `src/ScratchBird.Data/ScratchBirdConnection.cs:1001`, `src/ScratchBird.Data/MetadataPayloads.cs:10`, `src/ScratchBird.Data/Metadata.cs:26`, `src/ScratchBird.Data/Config.cs:22`, `tests/ScratchBird.Data.Tests/ScratchBirdConnectionMetadataShapingTests.cs:19`, `tests/ScratchBird.Data.Tests/ScratchBirdConnectionMetadataShapingTests.cs:56`, `tests/ScratchBird.Data.Tests/ScratchBirdConnectionMetadataShapingTests.cs:80`, `tests/ScratchBird.Data.Tests/ConfigTests.cs:71` | `GetSchema` covers extended families with collection-scoped restriction filtering, escaped wildcard literal handling, explicit `"null"` matching, and optional schema-parent expansion. DDL/editor parity is now exposed via typed schema payload/tree models with deterministic shaping tests. |
| `TYPE` | `JDBCBL-TYPE` | `Implemented` | `src/ScratchBird.Data/TypeDecoder.cs:99`, `src/ScratchBird.Data/TypeDecoder.cs:377`, `src/ScratchBird.Data/TypeDecoder.cs:583`, `src/ScratchBird.Data/TypeDecoder.cs:1084`, `src/ScratchBird.Data/ScratchBirdDataReader.cs:156`, `tests/ScratchBird.Data.Tests/TypeDecoderTests.cs:19`, `tests/ScratchBird.Data.Tests/TypeDecoderTests.cs:114`, `tests/ScratchBird.Data.Tests/TypeDecoderTests.cs:138`, `tests/ScratchBird.Data.Tests/IntegrationTests.cs:57`, `tests/ScratchBird.Data.Tests/IntegrationTests.cs:290` | Type decoder coverage now includes `timetz` wrapper encode/decode and expanded network-address text/binary cases (`inet`, `cidr`, `macaddr`, `macaddr8`) alongside existing JSON/UUID/numeric/range/composite mappings and OID/CLR validation. |
| `ERR` | `JDBCBL-ERR` | `Implemented` | `src/ScratchBird.Data/Errors.cs:111`, `src/ScratchBird.Data/Errors.cs:130`, `src/ScratchBird.Data/ProtocolClient.cs:1170`, `tests/ScratchBird.Data.Tests/ErrorStateMappingTests.cs:16`, `tests/ScratchBird.Data.Tests/ErrorStateMappingTests.cs:47` | SQLSTATE exact and class-prefix mappings are implemented and verified with unit tests, and the lane now exposes explicit retry-boundary helpers (`RetryScopeForSqlState`, `IsRetryableSqlState`). Query errors are routed through the mapper. |
| `RES` | `JDBCBL-RES` | `Implemented` | `src/ScratchBird.Data/ProtocolClientPool.cs:63`, `src/ScratchBird.Data/ProtocolClientPool.cs:190`, `src/ScratchBird.Data/ProtocolClientPool.cs:244`, `src/ScratchBird.Data/ProtocolClient.cs:1248`, `src/ScratchBird.Data/ScratchBirdDataReader.cs:341`, `tests/ScratchBird.Data.Tests/IntegrationTests.cs:743`, `tests/ScratchBird.Data.Tests/IntegrationTests.cs:777`, `tests/ScratchBird.Data.Tests/IntegrationTests.cs:853`, `tests/ScratchBird.Data.Tests/JDBC203PoolingAndRecoveryContractTests.cs:102`, `tests/ScratchBird.Data.Tests/TransactionExecutionParityTests.cs` | Lease/pool lifecycle, cancellation cleanup, saturation fallback, lifetime eviction, dormant fail-closed surfaces, and suspended-only portal-resume enforcement are implemented with deterministic tests. |
