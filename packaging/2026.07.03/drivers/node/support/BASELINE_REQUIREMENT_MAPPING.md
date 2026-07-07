# Node Baseline Requirement Mapping

Mapping of NODEBL groups to JDBC baseline groups for the Node lane.

Note: this mapping records the `2026-04-03` JDBC/.NET-class baseline closure.
The shared auth/bootstrap ratchet introduced on `2026-04-17` is broader and
is tracked through:

- `public_contract_snapshot`
- `public_contract_snapshot`
- `public_contract_snapshot`

Current lane note:

- the public Node lane now exposes staged auth-surface probing through
  `probeAuthSurface()`
- native direct-ingress execution now covers `PASSWORD`,
  `SCRAM_SHA_256`, `SCRAM_SHA_512`, and generic `TOKEN`
- admitted-but-not-local methods such as `MD5` and `PEER` fail closed through
  explicit unsupported/broker-required behavior
- live server verification and release evidence remain the outstanding closure
  step

## MGA Recovery Contract

- This lane follows ScratchBird's MGA/state-based engine recovery model.
- Reconnect or reopen only repairs transport and session state.
- Reconnect never resurrects abandoned in-flight transactions or replay lost statements.
- Transaction recovery in the lane means reset, rollback, reopen, or retry against engine truth.
- Result resume is valid only for explicit suspended protocol states.
- Internal portal resume now fails closed with `55000` unless the server first
  reported `PORTAL_SUSPENDED`.
- `prepareTransaction(...)`, `commitPrepared(...)`, and
  `rollbackPrepared(...)` expose explicit prepared/limbo control through
  canonical transaction-control SQL.
- `supportsDormantReattach()` is explicit and true on the native public lane;
  `detachToDormant()` returns the engine-issued `dormantId` plus
  `reattachToken`, and `reattachDormant(...)` uses those same explicit
  tokens through the public/native startup contract instead of implying
  reconnect-based recovery.
- `beginTransaction(options)` exposes the canonical MGA begin flags for
  `isolationLevel`, `accessMode`, `deferrable`, `wait`, `timeoutMs`,
  `autocommitMode`, `conflictAction`, and `readCommittedMode`.
- Native `READY`, `TXN_STATUS`, and `current_txn_id` are authoritative
  transaction-state surfaces; ScratchBird sessions stay always in a
  transaction and `COMMIT` / `ROLLBACK` immediately reopen the next boundary.
- Native autocommit transitions stay local to the wrapper instead of sending
  `SET_OPTION autocommit` or a synthetic replacement `BEGIN`.
- Current isolation alias mapping is explicit in lane source:
  `READ COMMITTED` => canonical `READ COMMITTED`,
  `REPEATABLE READ` => canonical `SNAPSHOT`,
  `SERIALIZABLE` => canonical `SNAPSHOT TABLE STABILITY`.
- `READ_COMMITTED_MODE_*` plus `canonicalReadCommittedModeLabel(...)` make the
  canonical `READ COMMITTED` sub-modes explicit in lane source, including the
  direct `READ COMMITTED READ CONSISTENCY` selector.
- `retryScopeForSqlState(...)` makes the retry boundary explicit:
  `40001`/`40P01` => statement only, `08xxx` => reconnect only, all other
  SQLSTATEs => no automatic replay.
- See `../../../../public_audit_summary`.

| NODEBL group | JDBC baseline group | Current status | Evidence anchors (lane source/tests) |
| --- | --- | --- | --- |
| `CONN` | `JDBCBL-CONN` | Implemented (core DSN, TLS, handshake, and manager proxy connect paths) | `src/client.ts`; `src/dsn.ts`; `test/unit.test.js`; `test/integration.test.js` |
| `TXN` | `JDBCBL-TXN` | Implemented (explicit begin/commit/rollback/savepoint lifecycle, canonical MGA begin-option payloads for isolation/read-committed-mode/access/deferrable/wait/timeout/autocommit/conflict-action, native `READY`/`TXN_STATUS`/`current_txn_id` transaction-state handling for active-with-zero-`txn_id` fresh boundaries, native autocommit transitions that stay local instead of sending `SET_OPTION autocommit`, invalid-state guards, and env-gated integration coverage for savepoint and implicit transaction flows) | `src/client.ts`; `src/protocol.ts`; `test/unit.test.js`; `test/integration.test.js` |
| `EXEC` | `JDBCBL-EXEC` | Implemented (simple/prepared/multi/batch/callable/generated-key execution paths, normalization error typing, stream paging, and env-gated integration coverage for streaming and multi-result surfaces) | `src/client.ts`; `src/sql.ts`; `test/unit.test.js`; `test/integration.test.js` |
| `META` | `JDBCBL-META` | Implemented (sys.* metadata routing now includes schema/table joins, restriction-aware filtering, JDBC-compatible alias shaping, recursive schema expansion/tree helpers, and env-gated metadata integration assertions) | `src/client.ts`; `src/metadata.ts`; `src/dsn.ts`; `test/unit.test.js`; `test/integration.test.js` |
| `TYPE` | `JDBCBL-TYPE` | Implemented (expanded type codec coverage includes explicit typed-OID encoding and broad decode parity across scalar, network, XML, text-search, geometry, range, composite, vector, and unknown fallback families with dedicated lane tests) | `src/types.ts`; `test/unit.test.js`; `test/types_parity.test.js`; `test/integration.test.js` |
| `ERR` | `JDBCBL-ERR` | Implemented (typed error classes, SQLSTATE mapping, and explicit retry-boundary helpers are covered, normalization failures map to `ScratchbirdSyntaxError` (`07001`), and protocol error translation now has dedicated tests for typed-class selection, `DETAIL/HINT` message shaping, empty-message fallback, parser-failure fallback behavior, and retry-scope classification) | `src/errors.ts`; `src/client.ts`; `test/unit.test.js`; `test/error_parity.test.js`; `test/integration.test.js` |
| `RES` | `JDBCBL-RES` | Implemented (resource lifecycle, pooling, cancellation, and resilience primitives are present with deterministic circuit-breaker/keepalive/leak-detection/telemetry unit coverage, and same-client reconnect now drops abandoned prepared/session caches before the replacement handshake) | `src/client.ts`; `src/circuit_breaker.ts`; `src/keepalive.ts`; `src/leak_detector.ts`; `src/telemetry.ts`; `test/unit.test.js`; `test/integration.test.js` |
