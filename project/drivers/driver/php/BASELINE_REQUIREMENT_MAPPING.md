# PHP S0 Baseline Requirement Mapping

Status key:
- Implemented: behavior exists with direct lane test evidence.
- Partial: behavior exists but has explicit limits and/or thin test coverage in this lane.

## MGA Recovery Contract

- This lane follows ScratchBird's MGA/state-based engine recovery model.
- Reconnect or reopen only repairs transport and session state.
- Reconnect never resurrects abandoned in-flight transactions or replay lost statements.
- Transaction recovery in the lane means reset, rollback, reopen, or retry against engine truth.
- Result resume is valid only for explicit suspended protocol states.
- `Connection::resumePortal()` now fails closed with `55000` unless the server
  first reported `MSG_PORTAL_SUSPENDED`.
- `Connection::prepareTransaction()`, `::commitPrepared()`, and
  `::rollbackPrepared()` expose explicit prepared/limbo control through
  canonical transaction-control SQL.
- `Connection::supportsDormantReattach()` is explicit and false, and
  `Connection::detachToDormant()` / `::reattachDormant()` fail closed with
  `0A000` until a public dormant front-door exists.
- native `READY`, `TXN_STATUS`, and `current_txn_id` are authoritative
  transaction-state surfaces; ScratchBird sessions stay always in a
  transaction and `COMMIT` / `ROLLBACK` immediately reopen the next boundary.
- `Connection::beginTransactionEx(array $options)` exposes the canonical
  `READ COMMITTED` sub-mode selector directly through
  `read_committed_mode`, including `READ COMMITTED READ CONSISTENCY`.
- `beginTransaction()` / `beginTransactionEx(...)` restart the current
  default boundary with the requested settings; only overlapping
  caller-owned explicit transaction state is rejected.
- commit and rollback now drain the immediate reopen boundary before the next
  operation so the follow-on statement sees real result frames rather than a
  stray reopen `READY`.
- `Protocol::canonicalReadCommittedModeLabel(...)` keeps that selector
  source-visible for auditors and lane tests.
- `ErrorMapper::retryScopeForSqlState(...)` makes the retry boundary explicit:
  `40001`/`40P01` => statement only, `08xxx` => reconnect only, all other
  SQLSTATEs => no automatic replay.
- See `../../../../public_audit_summary`.

| PHPBL group | JDBC baseline group | Current status | Evidence anchors (lane source/tests) |
| --- | --- | --- | --- |
| CONN | JDBCBL-CONN | Implemented | [src/Config.php](src/Config.php), [src/Connection.php](src/Connection.php), [src/Protocol.php](src/Protocol.php), [src/Scram.php](src/Scram.php), [tests/ConfigTest.php](tests/ConfigTest.php), [tests/ProtocolConnAuthTest.php](tests/ProtocolConnAuthTest.php), [tests/ConnectionConnTest.php](tests/ConnectionConnTest.php), [tests/ScramTest.php](tests/ScramTest.php), [tests/IntegrationTest.php](tests/IntegrationTest.php) |
| TXN | JDBCBL-TXN | Implemented | [src/Connection.php](src/Connection.php), [src/ScratchBirdPDO.php](src/ScratchBirdPDO.php), [tests/ConnectionTxnExecTest.php](tests/ConnectionTxnExecTest.php), [tests/IntegrationTest.php](tests/IntegrationTest.php) |
| EXEC | JDBCBL-EXEC | Implemented | [src/Connection.php](src/Connection.php), [src/Statement.php](src/Statement.php), [src/ResultStream.php](src/ResultStream.php), [src/Sql.php](src/Sql.php), [src/ScratchBirdPDO.php](src/ScratchBirdPDO.php), [tests/ConnectionTxnExecTest.php](tests/ConnectionTxnExecTest.php), [tests/SqlTest.php](tests/SqlTest.php), [tests/IntegrationTest.php](tests/IntegrationTest.php) |
| META | JDBCBL-META | Implemented | [src/Metadata.php](src/Metadata.php), [src/Connection.php](src/Connection.php), [src/ScratchBirdPDO.php](src/ScratchBirdPDO.php), [tests/MetadataRecursiveSchemaTest.php](tests/MetadataRecursiveSchemaTest.php), [tests/MetadataExecutionTest.php](tests/MetadataExecutionTest.php), [tests/IntegrationTest.php](tests/IntegrationTest.php) |
| TYPE | JDBCBL-TYPE | Implemented | [src/TypeDecoder.php](src/TypeDecoder.php), [src/ResultStream.php](src/ResultStream.php), [tests/TypeDecoderTest.php](tests/TypeDecoderTest.php), [tests/ConnectionTxnExecTest.php](tests/ConnectionTxnExecTest.php), [tests/IntegrationTest.php](tests/IntegrationTest.php) |
| ERR | JDBCBL-ERR | Implemented | [src/Errors.php](src/Errors.php), [src/Connection.php](src/Connection.php), [tests/ErrorsTest.php](tests/ErrorsTest.php), [tests/ConnectionTxnExecTest.php](tests/ConnectionTxnExecTest.php), [tests/IntegrationTest.php](tests/IntegrationTest.php) |
| RES | JDBCBL-RES | Implemented | [src/ResultStream.php](src/ResultStream.php), [src/Statement.php](src/Statement.php), [src/Connection.php](src/Connection.php), [tests/ConnectionTxnExecTest.php](tests/ConnectionTxnExecTest.php), [tests/IntegrationTest.php](tests/IntegrationTest.php) |

## Notes on current status

- CONN: DSN alias parsing, native protocol/front-door normalization, TLS/direct and manager-proxy connect flows, compatibility connection options (`sslmode=disable`, `binary_transfer=false`, `compression=zstd`), staged `Connection::probeAuthSurface(...)`, resolved-auth reporting, manager fast-path/challenge-path flows, direct `PASSWORD` / `SCRAM_SHA_256` / `SCRAM_SHA_512` / `TOKEN` execution, fail-closed `MD5` / `PEER` / `REATTACH`, and typed auth/connection failures are covered by deterministic lane tests plus env-gated integration probes.
- TXN: Begin/commit/rollback/savepoint/release/rollback-to semantics and transaction guard behavior are implemented with `READY` / transaction-status-driven state synchronization, including active-with-zero-`txn_id` fresh-boundary adoption, server-error-plus-ready abort handling, immediate reopen-boundary draining, and live transaction lifecycle checks. The direct lane surface now exposes `beginTransactionEx(...)` with `read_committed_mode` for canonical `READ COMMITTED` sub-mode parity, while standard `beginTransaction()` remains the default convenience subset.
- EXEC: SQL normalization/callable execution, batch summaries, multi-result traversal, generated keys, portal suspend/resume continuation, and statement rowset traversal are covered by deterministic wire-fixture tests and env-gated runtime checks.
- META: Metadata collection mapping, alias normalization, recursive schema-tree shaping, restriction filtering, and exposed PDO wrappers (`getSchema`, `getSchemaTree`) are implemented with lane tests and live-shape integration probes.
- TYPE: Type encode/decode coverage includes core scalar, temporal, JSON/JSONB, UUID, monetary/numeric, range/composite, geometry, and representative runtime roundtrip checks with expanded OID matrix assertions.
- ERR: SQLSTATE-class and SQLSTATE-specific exception mapping, explicit retry-boundary helpers, wire detail/hint propagation, and connection/statement errorInfo paths are validated in dedicated lane tests and integration error mapping checks.
- RES: Result-stream lifecycle, multi-result boundaries, cursor completion, close semantics, and connection resource cleanup are validated through deterministic execution tests and integration coverage.
