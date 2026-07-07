# ScratchBird Driver Baseline Requirement Mapping (S0)

## Scope

- Lane-local S0 artifact for `lanes/active/drivers/dart` only.
- Mapping is based only on source and tests in this lane.
- This file does not declare cross-lane or canonical spec authority.

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
- `readCommittedModeReadConsistency` plus
  `canonicalReadCommittedModeLabel(...)` make the canonical
  `READ COMMITTED` sub-modes explicit in lane source, including the direct
  `READ COMMITTED READ CONSISTENCY` selector.
- `retryScopeForSqlState(...)` makes the retry boundary explicit:
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
  `_allowPortalResume()` and `_resumeSuspendedPortal(...)`, which reject blind
  resume with `55000` unless `MessageType.portalSuspended` was observed first.
- See `../../../../public_audit_summary`.

## CONN (JDBCBL)

- Current status: Implemented
- Lane-local source anchors:
  - `lib/src/config.dart` DSN parsing, staged auth/bootstrap config aliases, and front-door normalization.
  - `lib/src/auth_bootstrap.dart` shared auth/bootstrap type and helper surface.
  - `lib/src/protocol.dart` auth method ids plus auth request/continue/ok parsing.
  - `lib/src/scram.dart` SHA-256 / SHA-512 SCRAM execution.
  - `lib/src/client.dart` staged probe path, resolved-auth reporting, direct/manager ingress, live `PASSWORD`/`SCRAM_SHA_256`/`SCRAM_SHA_512`/`TOKEN`, and fail-closed `MD5`/`PEER`/`REATTACH`.
- Lane-local test anchors:
  - `test/config_test.dart` DSN parsing, manager proxy parameters, invalid front-door validation, and staged auth/bootstrap parameter parsing.
  - `test/connect_validation_test.dart` connect reaches the socket layer for `sslmode=disable`, `binary_transfer=false`, and `compression=zstd`.
  - `test/auth_bootstrap_contract_test.dart` loopback direct probe, manager probe, `SCRAM_SHA_512`, `TOKEN`, and fail-closed `PEER` proof.
  - `test/integration_test.dart` live direct and manager-proxy connect/query smoke coverage (gated by `SCRATCHBIRD_TEST_DSN` and `SCRATCHBIRD_TEST_MANAGER_DSN`).
- Gaps/next actions:
  - Add manager-proxy integration coverage for handshake/auth/connect failure paths (invalid token, truncated manager frames, non-success MCP status).

## TXN (JDBCBL)

- Current status: Partial
- Lane-local source anchors:
  - `lib/src/protocol.dart:94-108` isolation, read-committed-mode, and transaction option flags.
  - `lib/src/protocol.dart:423-458` TXN payload builders (`begin`, `commit`, `rollback`, savepoint operations).
  - `lib/src/client.dart:23-40` public canonical read-committed-mode selector surface.
  - `lib/src/client.dart:423-470` client TXN APIs with active/inactive transaction guardrails.
  - `lib/src/client.dart:520-559` explicit prepared / limbo and dormant fail-closed helper surface.
  - `lib/src/client.dart:646-670` async `txnStatus` handling updates local transaction id state.
  - `lib/src/client.dart:778-824` ready-state txn tracking and TXN guard helper methods.
- Lane-local test anchors:
  - `test/txn_exec_parity_test.dart:19-60` TXN guardrail checks (`commit`/`rollback`/`savepoint` require active transaction).
  - `test/txn_exec_parity_test.dart:76-145` prepared transaction canonical SQL plus dormant fail-closed coverage.
  - `test/txn_exec_parity_test.dart:63-145` TXN payload encoding coverage for begin, read-committed-mode, and savepoint/release/rollback-to payloads.
  - `test/integration_test.dart:117-166` live begin/commit/rollback/savepoint lifecycle and nested-begin rejection coverage (gated by `SCRATCHBIRD_TEST_DSN`).
- Gaps/next actions:
  - Add live integration tests for server-side TXN failure paths (invalid savepoint, conflict, forced rollback conditions).

## EXEC (JDBCBL)

- Current status: Partial
- Lane-local source anchors:
  - `lib/src/client.dart:308-320` `query` entrypoint with SQL-empty guard.
  - `lib/src/client.dart:494-500` `cancel` rejects when no active in-flight sequence is tracked.
  - `lib/src/client.dart:573-644` result collection and send paths with pagination resume and query-sequence reset on terminal outcomes.
  - `lib/src/client.dart:793-795`, `lib/src/client.dart:1134-1152` suspended-only resume guard and execution path.
  - `lib/src/client.dart:460-468` SBLR execution path.
  - `lib/src/protocol.dart:199-302` query/parse/bind/execute/SBLR payload builders.
- Lane-local test anchors:
  - `test/txn_exec_parity_test.dart:104-131` EXEC guardrail checks (`query` empty SQL rejection, cancel-without-inflight rejection).
  - `test/txn_exec_parity_test.dart:274-291` suspended-only resume fail-closed coverage.
  - `test/txn_exec_parity_test.dart:134-171` EXEC payload encoding coverage for query/execute/cancel payload contracts.
  - `test/integration_test.dart:50-96` live simple and parameterized query coverage (gated by `SCRATCHBIRD_TEST_DSN` and `SCRATCHBIRD_TEST_MANAGER_DSN`).
- Gaps/next actions:
  - Add integration tests for pagination (`portalSuspended` path) and SBLR execution.
  - Add focused execution tests for async message capture paths (`queryPlan`, `notification`, `sblrCompiled`) under live wire flow.

## META (JDBCBL)

- Current status: Partial
- Lane-local source anchors:
  - `lib/src/metadata.dart:9-66` catalog query constants now cover schemas, tables, columns, indexes, index columns, constraints, procedures, functions, routines, catalogs, primary keys, foreign keys, table privileges, column privileges, and type info.
  - `lib/src/metadata.dart:54-195` metadata collection normalization + query resolution helpers, including richer alias families (`catalogs`, `primary_keys`, `foreign_keys`, `table_privileges`, `column_privileges`, `type_info`, `routines`).
  - `lib/src/metadata.dart:137-215` metadata-only recursive schema tree shaping (`expandParents`, database root, per-parent uniqueness).
  - `lib/src/metadata.dart:217-360` metadata row shaping with optional dotted-parent expansion and catalog-preserving synthetic parent rows.
  - `lib/src/client.dart:557-621` richer client metadata wrapper APIs (`metadataSchemas`, `metadataTables`, `metadataColumns`, `metadataIndexes`, `metadataIndexColumns`, `metadataConstraints`, `metadataProcedures`, `metadataFunctions`, `metadataRoutines`, `metadataCatalogs`, `metadataPrimaryKeys`, `metadataForeignKeys`, `metadataTablePrivileges`, `metadataColumnPrivileges`, `metadataTypeInfo`), plus `getSchema` and `getSchemaTree`.
  - `lib/scratchbird.dart:14` metadata export.
- Lane-local test anchors:
  - `test/metadata_execution_test.dart:28-174` metadata query alias resolution, richer metadata wrapper routing, and runtime schema expansion/tree APIs.
  - `test/metadata_recursive_schema_test.dart:6-33` database->default-branch style metadata rows and dotted parent expansion behavior.
  - `test/metadata_recursive_schema_test.dart:35-58` dotted schema parent expansion ordering/uniqueness in path extraction.
  - `test/metadata_recursive_schema_test.dart:60-79` per-parent uniqueness for duplicate leaf paths.
  - `test/metadata_recursive_schema_test.dart:81-107` same leaf name under different parents remains distinct in recursive schema tree.
  - `test/integration_test.dart:168-186` live metadata wrapper execution coverage (gated by `SCRATCHBIRD_TEST_DSN`).
- Gaps/next actions:
  - Add live metadata integration coverage for restrictions/wildcards and DDL-editor payload fields.
  - Add live proof for the richer catalog families (keys, privileges, type-info, and routine/cross-catalog surfaces), since the local wrapper/query breadth is now in place.

## TYPE (JDBCBL)

- Current status: Partial
- Lane-local source anchors:
  - `lib/src/types.dart:14-49` OID/type constants.
  - `lib/src/types.dart:143-377` parameter encoding and value decoding core logic.
  - `lib/src/types.dart:413-670` range/composite encode/decode handling.
  - `lib/src/types.dart:672-760` unknown-binary/text coercion and array literal parsing.
- Lane-local test anchors:
  - `test/type_mapping_test.dart:38-101` array/vector/range/composite/inet-cidr-macaddr round-trip coverage.
  - `test/type_mapping_test.dart:103-230` scalar decode coverage, text-vs-unknown decode behavior, and negative-path range/composite/unsupported-type checks.
  - `test/integration_test.dart:98-115` live scalar type round-trip smoke coverage (gated by `SCRATCHBIRD_TEST_DSN`).
  - `test/integration_test.dart:188-220` live json/jsonb round-trip coverage (gated by `SCRATCHBIRD_TEST_DSN`).
- Gaps/next actions:
  - Add live integration tests validating binary wire round-trip behavior for complex types (range/composite/vector/inet-cidr-macaddr).

## ERR (JDBCBL)

- Current status: Partial
- Lane-local source anchors:
  - `lib/src/errors.dart:9-204` typed driver exception hierarchy plus SQLSTATE-to-exception mapping (`mapSqlStateExecutionException`, `mapSqlStateAuthException`).
  - `lib/src/protocol.dart:145-237` structured protocol error parsing/formatting (`parseErrorMessage`, `formatProtocolErrorMessage`) with optional numeric code extraction.
  - `lib/src/client.dart:123-317` connect-time option rejection and manager-proxy auth/protocol failure mapping.
  - `lib/src/client.dart:549-671` execution/auth failure mapping (`cancel`, handshake auth, query failure).
  - `lib/src/client.dart:752-815`, `857-898`, `913-923`, `976-999`, `1086` protocol/transaction/socket/circuit error mapping.
  - `lib/src/scram.dart:34-63` SCRAM nonce/signature failures mapped to `ScratchBirdAuthException`.
  - `lib/src/protocol.dart:59` `MessageType.error` constant.
- Lane-local test anchors:
  - `test/error_resilience_test.dart:38-78` protocol framing tests assert `ScratchBirdProtocolException`.
  - `test/error_resilience_test.dart:81-122` structured server-error parsing/formatting tests.
  - `test/error_sqlstate_mapping_test.dart:5-68` SQLSTATE class mapping tests for execution/auth exception subclasses plus retry-scope classification.
  - `test/connect_validation_test.dart:29-69` connect-policy rejection tests assert `ScratchBirdConnectionException`.
  - `test/txn_exec_parity_test.dart:20-60,119-131` TXN and cancel guardrails assert typed transaction/execution exceptions.
  - `test/scram_error_test.dart:6-35` SCRAM nonce/signature failures assert `ScratchBirdAuthException`.
- Gaps/next actions:
  - Extend SQLSTATE/code mapping to manager-proxy MCP error payloads (non-`MessageType.error` path) so those failures include normalized `sqlState`/`code` metadata.
  - Add integration coverage that validates parsed SQLSTATE/code propagation from real server `MessageType.error` payloads.

## RES (JDBCBL)

- Current status: Partial
- Lane-local source anchors:
  - `lib/src/circuit_breaker.dart:9-114` circuit-breaker implementation.
  - `lib/src/keepalive.dart:11-87` idle validation and periodic ping orchestration.
  - `lib/src/leak_detector.dart:11-95` connection leak tracking/guarding with timer callback hook (`onLeakDetected`).
  - `lib/src/telemetry.dart:11-113` tracing/metrics/slow-query collection with retention/read-only accessors.
  - `lib/src/client.dart:1053-1103` resilience integration (`_startResilience`, `_stopResilience`, `_withResilience`).
- Lane-local test anchors:
  - `test/error_resilience_test.dart:126-177` circuit-breaker transition and recovery tests.
  - `test/error_resilience_test.dart:179-220` keepalive tracker/manager validation and ping trigger tests.
  - `test/error_resilience_test.dart:223-293` leak detector guard/stack-capture and timer callback coverage.
  - `test/error_resilience_test.dart:295-361` telemetry tracing/metrics/sanitization and slow-query retention coverage.
- Gaps/next actions:
  - Add integration tests covering idle-validation ping against live sockets and resilience state cleanup on client close.
