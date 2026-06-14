# ScratchBird ODBC Driver (ODBC 3.8)

ODBC 3.8 driver for ScratchBird SBWP v1.1.

## Documentation

- Getting started
- API reference
- Connectivity guide
- Shared auth/bootstrap contract
- [Baseline mapping](BASELINE_REQUIREMENT_MAPPING.md)

## MGA Recovery Contract

This lane follows ScratchBird's MGA/state-based engine recovery model.

- reconnect or reopen only repairs transport and session state
- reconnect never resurrects abandoned in-flight transactions or replay lost statements
- transaction recovery in the lane means reset, rollback, reopen, or retry against engine truth
- result resume is valid only for explicit suspended protocol states
- disconnect clears statement handles, prepared SQL cache, transaction flags,
  and bridge session state before any later reconnect
- the focused live recovery closure test in
  `tests/test_odbc_external_runtime.cpp` now proves that rollback leaves the
  next query immediately usable on the reopened native MGA boundary with no
  reconnect and no statement replay
- `SQL_ATTR_TXN_ISOLATION` still exposes only ODBC's standard isolation flags;
  the lane now documents their canonical MGA meaning directly in source:
  `READ UNCOMMITTED` remains a legacy compatibility alias,
  `READ COMMITTED` => canonical `READ COMMITTED`,
  `REPEATABLE READ` and `VERSIONING` => canonical `SNAPSHOT`,
  `SERIALIZABLE` => canonical `SNAPSHOT TABLE STABILITY`
- a distinct public selector for `READ COMMITTED READ CONSISTENCY` is not yet
  exposed through the ODBC transaction attribute surface
- retry remains SQLSTATE-driven and fail-closed in this lane:
  `40001`/`40P01` require a fresh statement boundary,
  `08xxx` requires reconnect or reopen, and nothing auto-replays a whole
  transaction
- the ODBC retry helper now inspects the primary diagnostic SQLSTATE and stops
  immediately on operator-intervention/governance outcomes such as `57014`
- prepared / limbo truth is explicit in lane source through
  `supportsPreparedTransactions()` plus `buildPreparedTransactionSql(...)`
  rather than implied by reconnect behavior
- dormant reattach truth is explicit and fail-closed through
  `supportsDormantReattach()` and `rejectDormantReattach(...)`
- standalone portal resume is intentionally absent and source-visible through
  `supportsPortalResume() -> false`

See `../../../../public_audit_summary`.

## Platform Support

| Platform | Status | Notes |
|----------|--------|-------|
| Linux | Supported | CI build coverage. |
| Windows | Supported | CI build coverage. |
| macOS | Untested | Not currently covered in CI. |

## Build

```bash
cmake -S . -B build
cmake --build build --config Release
```

See `docs/BUILD_MATRIX.md` for required ODBC/OpenSSL dependencies.

## Connection Strings

Direct/native:

```ini
Driver={ScratchBird};Server=127.0.0.1;Port=3092;Database=mydb;UID=user;PWD=pass;SSLMode=prefer
```

Direct/native token-auth bootstrap:

```ini
Driver={ScratchBird};Server=127.0.0.1;Port=3092;Database=mydb;UID=user;AuthMethodId=scratchbird.auth.token;AuthToken=token
```

Manager-proxy:

```ini
Driver={ScratchBird};Server=127.0.0.1;Port=3092;Database=mydb;UID=user;PWD=pass;FrontDoorMode=manager_proxy;ManagerAuthToken=token
```

## Staged Auth / Bootstrap

This lane now implements the shared driver auth/bootstrap contract through the
native ODBC bridge layer.

- `ConnectionParams` accepts generic `AuthToken` plus dormant reattach inputs.
- the connection-string / DSN parser accepts `AuthToken`, `BearerToken`, and
  `Token` aliases and normalizes them into the bridge config
- `OdbcClientBridge::probeAuthSurface(...)` performs staged preflight against
  direct or `manager_proxy` ingress before credential commitment
- `OdbcClientBridge::getResolvedAuthContext()` reports the truthfully resolved
  auth surface after probe, connect, and disconnect
- direct executable methods now include `PASSWORD`, `SCRAM_SHA_256`,
  `SCRAM_SHA_512`, and `TOKEN`
- `MD5`, `PEER`, and `REATTACH` are fail-closed when admitted but not locally
  executable through this lane

## Baseline Mapping

See [BASELINE_REQUIREMENT_MAPPING.md](BASELINE_REQUIREMENT_MAPPING.md) for S0 ODBCBL-to-JDBC baseline status and evidence anchors.
