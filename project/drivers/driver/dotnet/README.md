# ScratchBird .NET Driver

ScratchBird ADO.NET provider using the native wire protocol.

## Documentation

- Getting started
- API reference
- [Baseline requirement mapping](BASELINE_REQUIREMENT_MAPPING.md)

## Auth / Bootstrap Contract

This lane now implements the shared staged auth/bootstrap contract.

- public probe surface:
  - `ScratchBirdConnection.ProbeAuthSurface(connectionString)`
- resolved auth reporting:
  - `ScratchBirdConnection.GetResolvedAuthContext()`
  - `ProtocolClient.GetResolvedAuthContext()`
- direct listener and `manager_proxy` bootstrap
- executable local auth classes:
  - `PASSWORD`
  - `SCRAM_SHA_256`
  - `SCRAM_SHA_512`
  - `TOKEN`
- fail-closed negotiated reporting for admitted but unsupported or
  broker-required methods such as `MD5`, `PEER`, and `REATTACH`
- public connection-string builder coverage for:
  - `FrontDoorMode`
  - `AuthToken`
  - auth hint/pinning fields
  - `ManagerAuthToken`

## MGA Recovery Contract

This lane follows ScratchBird's MGA/state-based engine recovery model.

- reconnect or reopen only repairs transport and session state
- reconnect never resurrects abandoned in-flight transactions or replay lost statements
- transaction recovery in the lane means reset, rollback, reopen, or retry against engine truth
- result resume is valid only for explicit suspended protocol states
- `PrepareTransaction(...)`, `CommitPrepared(...)`, and
  `RollbackPrepared(...)` now expose explicit prepared / limbo control
  surfaces through canonical transaction-control SQL
- `SupportsDormantReattach() -> true`, `DetachToDormant()`, and
  `ReattachDormant(...)` now expose the explicit dormant token flow on the
  native public lane, with engine-issued `dormant_id` plus
  `dormant_reattach_token` carried through the public startup contract
- `BeginTransaction(ScratchBirdTransactionOptions)` exposes the canonical MGA
  begin flags for `IsolationLevel`, `AccessMode`, `Deferrable`, `Wait`,
  `TimeoutMs`, `AutoCommit`, and `ReadCommittedMode`
- native `READY`, `TXN_STATUS`, and `current_txn_id` are treated as
  authoritative transaction-state surfaces; ScratchBird sessions stay always
  in a transaction and `COMMIT` / `ROLLBACK` reopen the next boundary
- `BeginTransaction(...)` restarts the current boundary with the requested
  options instead of assuming idle-session semantics
- current isolation alias mapping is explicit in lane source:
  `IsolationLevel.ReadCommitted` => canonical `READ COMMITTED`,
  `IsolationLevel.RepeatableRead` => canonical `SNAPSHOT`,
  `IsolationLevel.Serializable` / `Snapshot` / `Chaos` => canonical
  `SNAPSHOT TABLE STABILITY`
- `ScratchBirdReadCommittedMode` now exposes the canonical `READ COMMITTED`
  sub-modes directly; `ScratchBirdReadCommittedMode.ReadConsistency` selects
  canonical `READ COMMITTED READ CONSISTENCY`
- `ScratchBirdSqlStateMapper.RetryScopeForSqlState(...)` makes the retry
  boundary explicit: `40001`/`40P01` => fresh statement only, `08xxx` =>
  reconnect or reopen only, everything else => no automatic replay
- `ProtocolClient.ResumeSuspendedPortal(...)` now rejects unsuspended resume
  with `55000`, and the query/read paths only call it after
  `PORTAL_SUSPENDED`

See `../../../../public_audit_summary`.

## Build/Test (Windows/Linux)

See `docs/BUILD_MATRIX.md`.

## Platform Support

| Platform | Status | Notes |
|----------|--------|-------|
| Linux | Supported | CI build/test coverage. |
| Windows | Supported | CI build/test coverage. |
| macOS | Untested | Not currently covered in CI. |

## Build

```bash
dotnet build src/ScratchBird.Data/ScratchBird.Data.csproj
```

## Tests

```bash
dotnet test
```

Integration env:

- `SCRATCHBIRD_DOTNET_URL`
- when `SCRATCHBIRD_DOTNET_URL` is unset, live integration suites default to
  `scratchbird://sb_admin:SbAdmin_Compat1!@127.0.0.1:13092/main?sslmode=disable&allow_insecure=true`

## Enterprise soak/fault harnesses

Deterministic mode:

```bash
bash artifacts/enterprise-readiness/run_dotnet_soak_suite.sh

# or per-ticket:
bash artifacts/enterprise-readiness/DOTNET-101/verification_dotnet_soak.sh
bash artifacts/enterprise-readiness/DOTNET-102/verification_dotnet_failover_soak.sh
bash artifacts/enterprise-readiness/DOTNET-103/verification_dotnet_fault_matrix.sh
```

Runtime mode (requires live DSN):

```bash
DOTNET_HARNESS_MODE=runtime SCRATCHBIRD_DOTNET_URL='scratchbird://...' \
bash artifacts/enterprise-readiness/run_dotnet_soak_suite.sh

# or per-ticket:
DOTNET_HARNESS_MODE=runtime SCRATCHBIRD_DOTNET_URL='scratchbird://...' \
bash artifacts/enterprise-readiness/DOTNET-101/verification_dotnet_soak.sh

DOTNET_HARNESS_MODE=runtime SCRATCHBIRD_DOTNET_URL='scratchbird://...' \
bash artifacts/enterprise-readiness/DOTNET-102/verification_dotnet_failover_soak.sh

DOTNET_HARNESS_MODE=runtime SCRATCHBIRD_DOTNET_URL='scratchbird://...' \
bash artifacts/enterprise-readiness/DOTNET-103/verification_dotnet_fault_matrix.sh
```
