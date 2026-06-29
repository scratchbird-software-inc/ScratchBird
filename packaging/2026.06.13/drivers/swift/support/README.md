# ScratchBird Swift Driver

Native ScratchBird driver using Swift Concurrency (async/await). SBWP v1.1,
with staged auth/bootstrap probing, direct listener ingress, and
`manager_proxy` front-door support.

## Lane Docs

- [Baseline Requirement Mapping (S0)](./BASELINE_REQUIREMENT_MAPPING.md)
- Getting started
- API reference

## MGA Recovery Contract

This lane follows ScratchBird's MGA/state-based engine recovery model.

- reconnect or reopen only repairs transport and session state
- reconnect never resurrects abandoned in-flight transactions or replay lost statements
- transaction recovery in the lane means reset, rollback, reopen, or retry against engine truth
- result resume is valid only for explicit suspended protocol states
- `begin(...)` exposes the canonical MGA begin flags for `isolationLevel`,
  `readCommittedMode`, `accessMode`, `deferrable`, `wait`, `timeoutMs`,
  `autocommitMode`, and `conflictAction`
- current isolation alias mapping is explicit in lane source:
  `READ COMMITTED` => canonical `READ COMMITTED`,
  `REPEATABLE READ` => canonical `SNAPSHOT`,
  `SERIALIZABLE` => canonical `SNAPSHOT TABLE STABILITY`
- `ScratchBirdReadCommittedMode.readConsistency` now exposes the canonical
  `READ COMMITTED READ CONSISTENCY` selector directly in the public lane
- `retryScope(forSqlState:)` makes the retry boundary explicit:
  `40001`/`40P01` => fresh statement only, `08xxx` => reconnect or reopen
  only, everything else => no automatic replay
- native `READY`, `TXN_STATUS`, and `current_txn_id` are authoritative for
  transaction-state truth on the engine-endpoint lane, including fresh MGA
  boundaries that remain active while `txnId == 0`
- compatible default `begin(...)` calls adopt that already-active fresh native
  boundary, while unsupported non-default fresh-boundary adoption fails closed
  with `0A000`
- `commit(...)` and `rollback(...)` drain the immediate reopen boundary before
  the next operation so the first post-commit / post-rollback query sees real
  result frames rather than a stray reopen `READY`
- prepared / limbo truth is explicit in lane code through
  `supportsPreparedTransactions()`, `prepareTransaction(...)`,
  `commitPrepared(...)`, and `rollbackPrepared(...)`, which emit canonical
  transaction-control SQL
- dormant detach / reattach truth is explicit in lane code through
  `supportsDormantReattach() -> false`, `detachToDormant()`, and
  `reattachDormant(...)`, all of which fail closed with `0A000` until the
  public front door exposes a real dormant-token flow
- internal result continuation is gated by `allowPortalResume()` and
  `resumeSuspendedPortal(...)`, so blind resume fails closed with `55000`
  unless `PORTAL_SUSPENDED` was observed first

See `../../../../public_audit_summary`.

## Platform Support

| Platform | Status | Notes |
|----------|--------|-------|
| Linux | Supported | CI build/test coverage. |
| Windows | Not supported | Swift target/toolchain path is not configured for this repo. |
| macOS | Expected | SwiftPM workflow should work; not currently covered in CI. |

## Build (local dev)

```bash
cd swift
swift build
```

## Quick Start

```swift
import ScratchBird

let config = ScratchBirdConfig(dsn: "scratchbird://user:pass@localhost:3092/mydb")
let conn = try await ScratchBirdConnection.connect(config)
let result = try await conn.query("SELECT 1")
print(result.rows)
try await conn.close()
```

## Auth Bootstrap

The Swift lane now exposes the shared staged auth/bootstrap contract:

- `ScratchBirdConnection.probeAuthSurface(_:)`
- `ScratchBirdConnection.getResolvedAuthContext()`
- direct auth execution for `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`, and `TOKEN`
- fail-closed handling for `MD5`, `PEER`, and `REATTACH`
- `manager_proxy` bootstrap probing plus manager-token connect

Canonical auth/bootstrap DSN fields now include:

- `front_door_mode`
- `manager_auth_token`, `manager_username`, `manager_database`
- `auth_token`
- `auth_method_id`, `auth_method_payload`
- `auth_payload_json`, `auth_payload_b64`
- `auth_provider_profile`
- `auth_required_methods`, `auth_forbidden_methods`
- `auth_require_channel_binding`
- `workload_identity_token`, `proxy_principal_assertion`

## TLS Note

TLS is required and implemented for ScratchBird connections.

- Apple and Linux: TLS transport uses `NIOSSL` whenever certificate files are
  supplied (`sslrootcert`, `sslcert`, `sslkey`), otherwise `Network` is used on
  Apple platforms.

`sslmode` supports: `disable`, `allow`, `prefer`, `require`,
`verify-ca`, and `verify-full`.

`sslkey`/`sslpassword` are currently loaded through NIOSSL when present.

## Error Model

Wire errors are mapped into typed Swift exceptions by SQLSTATE class/exact code:

- `ScratchBirdConnectionException`
- `ScratchBirdAuthorizationException`
- `ScratchBirdDataException`
- `ScratchBirdIntegrityException`
- `ScratchBirdTransactionException`
- `ScratchBirdProgrammingException`
- `ScratchBirdNotSupportedException`
- `ScratchBirdTimeoutException`
- `ScratchBirdOperationalException`

All typed exceptions carry structured fields (`sqlState`, `severity`, `detail`,
`hint`) and preserve `NSError` compatibility via `errorUserInfo`.

Retry helpers are also exposed directly in lane source:

- `retryScope(forSqlState:)`
- `isRetryable(sqlState:)`

## Metadata Helpers

Connection-level metadata wrappers are available for `sys.*` catalog families:

- `metadataSchemas`, `metadataTables`, `metadataColumns`
- `metadataIndexes`, `metadataIndexColumns`, `metadataConstraints`
- `metadataProcedures`, `metadataFunctions`
- `metadataRoutines`, `metadataCatalogs`
- `metadataPrimaryKeys`, `metadataForeignKeys`
- `metadataTablePrivileges`, `metadataColumnPrivileges`
- `metadataTypeInfo`
- `metadataSchemaTree`, `metadataSchemaTreeRows`

The remaining metadata gap in this lane is live catalog-completeness proof
against a real engine target, not absence of the richer wrapper surface.

## Execution Helpers

- `executeBatch(sql, paramsBatch)` for sequential batch execution helper semantics.
- `queryMulti(statements)` for multi-statement helper execution.
- `executeReturningFirstColumn(sql, params)` for generated-key-style first-column extraction.

## Pooling

`ScratchBirdConnectionPool` provides lightweight checkout/release and
`withConnection` helpers for bounded connection reuse.

## Resilience Tuning

Optional DSN parameters:

- `keepalive_interval_ms`
- `keepalive_max_idle_before_check_ms`
- `keepalive_validation_timeout_ms`
- `leak_detection_threshold_ms`
- `leak_detection_check_interval_ms`
- `leak_detection_capture_stack_trace`

## Tests

Integration tests use:

- `SCRATCHBIRD_TEST_DSN`
- `SCRATCHBIRD_TEST_MANAGER_DSN`
- `SCRATCHBIRD_TEST_BAD_AUTH_DSN` (optional: DSN with intentionally invalid credentials for auth-failure mapping test)

Run all tests:

```bash
swift test
```

The default `swift test` suite now includes local staged auth/bootstrap
contract coverage for direct probe, manager probe, `SCRAM_SHA_512`, `TOKEN`,
and fail-closed unsupported auth methods without requiring a live ScratchBird
listener.

Run only env-gated integration coverage:

```bash
swift test --filter IntegrationTests
```

Integration coverage includes direct + manager connect/query, TXN/savepoint,
metadata wrappers, typed error mapping, and resilience timing checks
(single-connection and concurrent multi-connection), plus pool
checkout/release churn assertions.
