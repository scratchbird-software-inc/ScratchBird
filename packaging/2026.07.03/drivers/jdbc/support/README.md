# ScratchBird JDBC Driver

Pure Java (Type 4) driver for ScratchBird.

## Auth / Bootstrap Contract

The JDBC lane now implements the shared staged auth/bootstrap contract:

- public staged probe surface:
  - `SBDriver.probeAuthSurface(url, Properties)`
  - `SBConnection.probeAuthSurface(SBConnectionProperties)`
- resolved auth reporting:
  - `SBConnection.getResolvedAuthContext()`
  - `SBProtocolHandler.getResolvedAuthContext()`
- native execution coverage:
  - `PASSWORD`
  - `SCRAM_SHA_256`
  - `SCRAM_SHA_512`
  - `TOKEN`
- fail-closed admitted-method handling:
  - `MD5`
  - `PEER`
  - `REATTACH`
- manager-proxy bootstrap remains first-class and is now part of the staged
  probe/runtime contract rather than a side path

## Documentation

- Getting started
- API reference
- [Baseline requirement mapping](BASELINE_REQUIREMENT_MAPPING.md)

## MGA Recovery Contract

This lane follows ScratchBird's MGA/state-based engine recovery model.

- reconnect or reopen only repairs transport and session state
- reconnect never resurrects abandoned in-flight transactions or replay lost statements
- transaction recovery in the lane means reset, rollback, reopen, or retry against engine truth
- result resume is valid only for explicit suspended protocol states
- `SBConnection.prepareTransaction(...)`, `commitPrepared(...)`, and
  `rollbackPrepared(...)` now expose explicit prepared / limbo control
  surfaces through canonical transaction-control SQL
- `SBConnection.supportsDormantReattach() -> false`,
  `detachToDormant()`, and `reattachDormant(...)` make dormant truth explicit
  and fail closed with `0A000` until the public front door exposes a real
  dormant token flow
- `setTransactionIsolation(...)` makes the current alias mapping explicit:
  `READ COMMITTED` => canonical `READ COMMITTED`,
  `REPEATABLE READ` / `SNAPSHOT` => canonical `SNAPSHOT`,
  `SERIALIZABLE` => canonical `SNAPSHOT TABLE STABILITY`
- `TRANSACTION_READ_UNCOMMITTED` is retained only as a legacy compatibility
  alias and does not create a distinct canonical MGA mode
- `SBConnection.setReadCommittedMode(...)` and
  `SBConnection.setTransactionIsolation(int, int)` expose the canonical
  `READ COMMITTED` sub-mode selector directly for the driver-owned connection
  surface, including `READ COMMITTED READ CONSISTENCY`
- `SBConnection.canonicalReadCommittedModeLabel(...)` makes the selected
  canonical MGA mode visible in lane source and tests
- native `READY`, `TXN_STATUS`, and `current_txn_id` are authoritative for
  transaction activity in this lane; ScratchBird sessions stay always in a
  transaction and `COMMIT` / `ROLLBACK` reopen the next boundary
- begin/start-transaction surfaces are documented against that
  always-in-transaction contract rather than idle-session semantics
- the focused live recovery closure test
  `SBJdbcClosureParityTest.manualTransactionsRemainImmediatelyUsableAcrossCommitAndRollback()`
  now proves that savepoints, commit, rollback, and the first post-rollback
  query remain immediately usable on the live native lane
- `SBProtocolHandler.retryScopeForSqlState(...)` makes the retry boundary
  explicit: `40001`/`40P01` => fresh statement only, `08xxx` => reconnect or
  reopen only, everything else => no automatic replay
- `SBProtocolHandler.resumeSuspendedPortal(...)` now rejects unsuspended
  resume with `55000`, and the streaming cursor only arms it after
  `MSG_PORTAL_SUSPENDED`

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
./gradlew build
```

Windows:

```cmd
gradlew.bat build
```

## Tests

```bash
./gradlew test
```

Windows:

```cmd
gradlew.bat test
```

Integration env:

- `SCRATCHBIRD_JDBC_URL`
- `SCRATCHBIRD_JDBC_USER`
- `SCRATCHBIRD_JDBC_PASSWORD`
