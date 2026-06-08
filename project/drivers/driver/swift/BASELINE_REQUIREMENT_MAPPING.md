# ScratchBird Driver Baseline Requirement Mapping (S0)

## Scope

- Lane-local S0 artifact for `lanes/active/drivers/swift` only.
- Maps Swift lane capability areas to JDBCBL groups using evidence in this lane's source and tests.
- Not a cross-lane conformance statement.

## MGA Recovery Contract

- This lane follows ScratchBird's MGA/state-based engine recovery model.
- Reconnect or reopen only repairs transport and session state.
- Reconnect never resurrects abandoned in-flight transactions or replay lost statements.
- Transaction recovery in the lane means reset, rollback, reopen, or retry against engine truth.
- Result resume is valid only for explicit suspended protocol states.
- `begin(...)` exposes the canonical MGA begin flags for `isolationLevel`,
  `readCommittedMode`, `accessMode`, `deferrable`, `wait`, `timeoutMs`,
  `autocommitMode`, and `conflictAction`.
- Current isolation alias mapping is explicit in lane source:
  `READ COMMITTED` => canonical `READ COMMITTED`,
  `REPEATABLE READ` => canonical `SNAPSHOT`,
  `SERIALIZABLE` => canonical `SNAPSHOT TABLE STABILITY`.
- `ScratchBirdReadCommittedMode.readConsistency` now exposes the canonical
  `READ COMMITTED READ CONSISTENCY` selector directly in the public lane.
- `retryScope(forSqlState:)` makes the retry boundary explicit:
  `40001`/`40P01` => statement only, `08xxx` => reconnect only, all other
  SQLSTATEs => no automatic replay.
- Prepared / limbo truth is explicit in lane source through
  `supportsPreparedTransactions()`, `prepareTransaction(...)`,
  `commitPrepared(...)`, and `rollbackPrepared(...)`, which emit canonical
  transaction-control SQL.
- Dormant truth is explicit in lane source through
  `supportsDormantReattach() -> false`, `detachToDormant()`, and
  `reattachDormant(...)`, which all fail closed with `0A000`.
- Result continuation is explicitly suspended-only through
  `allowPortalResume()` and `resumeSuspendedPortal(...)`, which reject blind
  resume with `55000` unless `.portalSuspended` was observed first.
- See `../../../../public_audit_summary`.

## CONN (`JDBCBL-CONN`)

- Current status: `Implemented`
- Lane-local source anchors:
  - `Sources/ScratchBird/AuthBootstrap.swift` shared auth/bootstrap probe/result context surface, auth method/plugin mapping, and startup auth parameter application.
  - `Sources/ScratchBird/Config.swift` DSN/config parsing for `front_door_mode`, manager bootstrap keys, generic `auth_token`, and staged auth-selection parameters.
  - `Sources/ScratchBird/Connection.swift` staged `probeAuthSurface(_:)`, resolved-auth tracking, manager bootstrap/connect, direct auth probe, and live handshake execution for `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`, and `TOKEN`.
  - `Sources/ScratchBird/Protocol.swift` auth method ids plus wire parse helpers for `AUTH_REQUEST`, `AUTH_CONTINUE`, and `AUTH_OK`.
  - `Sources/ScratchBird/Scram.swift` SHA-256 and SHA-512 SCRAM execution.
  - `Sources/ScratchBird/Socket.swift:136-179`
  - `Sources/ScratchBird/Socket.swift:287-415`
- Lane-local test anchors:
  - `Tests/ScratchBirdTests/ConfigTests.swift` DSN/config parsing for staged auth/bootstrap fields.
  - `Tests/ScratchBirdTests/AuthBootstrapContractTests.swift` local loopback proof for direct probe, manager probe, `SCRAM_SHA_512`, `TOKEN`, and fail-closed `PEER`.
  - `Tests/ScratchBirdTests/IntegrationTests.swift:13-31` env-gated live handshake/connect/close coverage for direct and manager-proxy DSNs.
- Gaps/next actions:
  - Add failure-path live connection coverage (manager auth challenge failure, version/protocol mismatch, and socket-read teardown behavior).

## TXN (`JDBCBL-TXN`)

- Current status: `Implemented` (focused live MGA boundary certified)
- Lane-local source anchors:
  - `Sources/ScratchBird/Protocol.swift:38-43`
  - `Sources/ScratchBird/Protocol.swift:116-127`
  - `Sources/ScratchBird/Protocol.swift:325-369`
  - `Sources/ScratchBird/TxnExecValidation.swift:11-65`
  - `Sources/ScratchBird/Connection.swift:152-245`
  - `Sources/ScratchBird/Connection.swift:394-437` explicit prepared / limbo and dormant fail-closed helper surface.
  - `Sources/ScratchBird/Connection.swift` now treats native `READY`,
    `TXN_STATUS`, and `current_txn_id` as authoritative transaction-state
    surfaces, adopts compatible default fresh native boundaries in `begin(...)`,
    and drains immediate reopen boundaries after `commit(...)` / `rollback(...)`.
- Lane-local test anchors:
  - `Tests/ScratchBirdTests/TxnExecParityTests.swift:13-58`
  - `Tests/ScratchBirdTests/TxnExecParityTests.swift:73-145`
  - `Tests/ScratchBirdTests/IntegrationTests.swift:48-60` env-gated live begin/commit/rollback/savepoint lifecycle.
  - `Tests/ScratchBirdTests/TxnExecParityTests.swift` now covers active fresh
    boundary adoption plus non-default fresh-boundary fail-closed behavior.
  - `Tests/ScratchBirdTests/IntegrationTests.swift` now proves default
    fresh-boundary `begin(...)` adoption and real post-rollback query results
    on the live native lane.
- Gaps/next actions:
  - Broader execution-layer portal suspend / resume and cancellation timing
    parity still lives under `JDBCBL-EXEC`; the core TXN fresh-boundary gap is
    closed in this lane.

## EXEC (`JDBCBL-EXEC`)

- Current status: `Partial`
- Lane-local source anchors:
  - `Sources/ScratchBird/Connection.swift:127-137`
  - `Sources/ScratchBird/Connection.swift:140-171` helper execution APIs (`executeBatch`, `queryMulti`, `executeReturningFirstColumn`).
  - `Sources/ScratchBird/Connection.swift:284-295`
  - `Sources/ScratchBird/Connection.swift:333-339`
  - `Sources/ScratchBird/Connection.swift:392-449`
  - `Sources/ScratchBird/Connection.swift:695-698`, `Sources/ScratchBird/Connection.swift:940-955` suspended-only resume guard and execution path.
  - `Sources/ScratchBird/Protocol.swift:266-280`
  - `Sources/ScratchBird/TxnExecValidation.swift:51-65`
- Lane-local test anchors:
  - `Tests/ScratchBirdTests/TxnExecParityTests.swift:50-60`
  - `Tests/ScratchBirdTests/TxnExecParityTests.swift:95-145`
  - `Tests/ScratchBirdTests/IntegrationTests.swift:13-85` env-gated live simple/parameterized execution plus batch/multi/helper execution paths.
- Gaps/next actions:
  - Add live execution tests for cancellation timing and portal suspend/resume behavior.
  - Promote helper-level batch/multi/generated-key semantics to true wire-level parity (single-request multi-result traversal and generated-key metadata fidelity).

## META (`JDBCBL-META`)

- Current status: `Partial`
- Lane-local source anchors:
  - `Sources/ScratchBird/Metadata.swift:11-87` catalog query constants now cover schemas, tables, columns, indexes, index columns, constraints, procedures, functions, routines, catalogs, primary keys, foreign keys, table privileges, column privileges, and type info.
  - `Sources/ScratchBird/Metadata.swift:58-150` metadata collection name normalization + richer alias resolution for those catalog families.
  - `Sources/ScratchBird/Metadata.swift:153-352` metadata-only recursive schema tree shaping plus schema-name extraction (`metadataSchemaNames`, `metadataSchemaPathsForNavigation`, `buildMetadataSchemaTree`, `buildMetadataSchemaTreeRows`) with optional parent expansion and per-parent uniqueness.
  - `Sources/ScratchBird/Connection.swift:280-351` client-facing metadata query wrappers (`metadataSchemas`, `metadataTables`, `metadataColumns`, `metadataIndexes`, `metadataIndexColumns`, `metadataConstraints`, `metadataProcedures`, `metadataFunctions`, `metadataRoutines`, `metadataCatalogs`, `metadataPrimaryKeys`, `metadataForeignKeys`, `metadataTablePrivileges`, `metadataColumnPrivileges`, `metadataTypeInfo`) and schema-tree accessors (`metadataSchemaTree`, `metadataSchemaTreeRows`).
- Lane-local test anchors:
  - `Tests/ScratchBirdTests/MetadataRecursiveSchemaTests.swift:13-72` schema-name extraction/normalization paths (named-column and fallback-column modes) plus richer metadata alias/query resolution families.
  - `Tests/ScratchBirdTests/MetadataRecursiveSchemaTests.swift:47-62` database/default root row + top-branch metadata row shape.
  - `Tests/ScratchBirdTests/MetadataRecursiveSchemaTests.swift:64-74` dotted parent expansion behavior for schema navigation paths.
  - `Tests/ScratchBirdTests/MetadataRecursiveSchemaTests.swift:76-88` uniqueness within the same parent branch.
  - `Tests/ScratchBirdTests/MetadataRecursiveSchemaTests.swift:90-104` same leaf name preserved under different parents.
  - `Tests/ScratchBirdTests/IntegrationTests.swift:62-76` env-gated live metadata wrapper invocation + schema tree row shaping.
- Gaps/next actions:
  - Expand live metadata integration coverage to validate full catalog payload completeness (keys/privileges/types/DDL-editor families), not only schemas/tables/tree entry points.
  - Add live proof for the richer routine/catalog/key/privilege/type-info wrappers now present in lane source and local tests.

## TYPE (`JDBCBL-TYPE`)

- Current status: `Partial`
- Lane-local source anchors:
  - `Sources/ScratchBird/Types.swift:11-214`
  - `Sources/ScratchBird/Types.swift:216-289`
  - `Sources/ScratchBird/Types.swift:391-844`
  - `Sources/ScratchBird/Connection.swift:398`
  - `Sources/ScratchBird/Connection.swift:425-427`
- Lane-local test anchors:
  - `Tests/ScratchBirdTests/TypeMappingTests.swift:5-180`
- Gaps/next actions:
  - Add live integration codec coverage (wire roundtrip against engine fixtures for scalar, temporal, JSON, and advanced container types).

## ERR (`JDBCBL-ERR`)

- Current status: `Partial`
- Lane-local source anchors:
  - `Sources/ScratchBird/Protocol.swift:11-28`
  - `Sources/ScratchBird/Protocol.swift:197-300` (`parseErrorMessage`, `buildScratchBirdError`, `buildScratchBirdNSError`, structured SQLSTATE/detail/hint extraction).
  - `Sources/ScratchBird/Errors.swift:11-289` typed driver exception hierarchy and SQLSTATE exact/class mappers.
  - `Sources/ScratchBird/Config.swift:11-57`
  - `Sources/ScratchBird/Connection.swift:91-99`
  - `Sources/ScratchBird/Connection.swift:265-269` ping `.error` mapped with connection SQLSTATE default.
  - `Sources/ScratchBird/Connection.swift:389-393` auth `.error` mapped with authorization SQLSTATE default.
  - `Sources/ScratchBird/Connection.swift:423-427` query `.error` mapped with execution SQLSTATE default.
  - `Sources/ScratchBird/Connection.swift:802-806` drain/request `.error` mapped with execution SQLSTATE default.
  - `Sources/ScratchBird/Connection.swift:657-667`
  - `Sources/ScratchBird/Connection.swift:675-763`
  - `Sources/ScratchBird/Socket.swift:157-178`
  - `Sources/ScratchBird/Socket.swift:289-304`
  - `Sources/ScratchBird/Socket.swift:338`
- Lane-local test anchors:
  - `Tests/ScratchBirdTests/ConfigTests.swift:51-99`
  - `Tests/ScratchBirdTests/ErrorResilienceTests.swift:13-53` protocol header decode guardrails (`invalidHeader`, `unsupportedVersion`, `payloadTooLarge`).
  - `Tests/ScratchBirdTests/ErrorResilienceTests.swift:55-154` wire-error payload parsing, typed SQLSTATE mapping, structured SQLSTATE/detail/hint propagation, malformed-payload fallback assertions, and retry-scope classification.
  - `Tests/ScratchBirdTests/IntegrationTests.swift:78-132` env-gated live SQLSTATE propagation for execution failures plus optional bad-auth connect mapping (`SCRATCHBIRD_TEST_BAD_AUTH_DSN`).
- Gaps/next actions:
  - Expand live auth/connect `.error` propagation to include manager auth failures and explicit read-loop teardown paths.

## RES (`JDBCBL-RES`)

- Current status: `Partial`
- Lane-local source anchors:
  - `Sources/ScratchBird/CircuitBreaker.swift:11-116`
  - `Sources/ScratchBird/Keepalive.swift:11-153` keepalive tracker/manager plus validation stats (`KeepaliveManager.Stats`).
  - `Sources/ScratchBird/Telemetry.swift:11-108`
  - `Sources/ScratchBird/LeakDetector.swift:11-110` leak detector timer/guard plus leak stats (`LeakDetector.Stats`).
  - `Sources/ScratchBird/Pool.swift:11-118` bounded connection-pool checkout/release and `withConnection` churn surface.
  - `Sources/ScratchBird/Config.swift:140-293` DSN-configurable keepalive/leak tuning options.
  - `Sources/ScratchBird/Connection.swift:71-103` resilience component initialization from config.
  - `Sources/ScratchBird/Connection.swift:211-224` internal resilience debug snapshot (`debugResilienceStats`).
  - `Sources/ScratchBird/Connection.swift:554-616` operation-level idle validation and keepalive activity updates.
- Lane-local test anchors:
  - `Tests/ScratchBirdTests/ErrorResilienceTests.swift:55-99` deterministic circuit-breaker closed/open/half-open transition and reopen-on-failure behavior.
  - `Tests/ScratchBirdTests/ErrorResilienceTests.swift:102-140` keepalive idle-threshold checks plus manager-driven ping verification.
  - `Tests/ScratchBirdTests/ErrorResilienceTests.swift:142-170` leak detector guard idempotency and checkout metadata/stack capture.
  - `Tests/ScratchBirdTests/ErrorResilienceTests.swift:172-203` telemetry tracing-disabled gate, success/failure metrics accounting, SQL sanitization.
  - `Tests/ScratchBirdTests/ConfigTests.swift:41-54` resilience DSN option parsing.
  - `Tests/ScratchBirdTests/IntegrationTests.swift:131-250` env-gated live keepalive/leak assertions for single-connection and concurrent multi-connection workloads, plus pool checkout/release churn and exhaustion behavior.
- Gaps/next actions:
  - Expand pool behavior to include wait-queue/timeouts and explicit failure-recovery semantics under sustained saturation/fault injection.
