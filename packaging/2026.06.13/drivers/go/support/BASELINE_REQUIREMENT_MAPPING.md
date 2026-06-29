# Go Baseline Requirement Mapping (S0)

Scope: `lanes/active/drivers/go` lane only.

Status legend:
- `Implemented`: baseline-complete coverage exists with lane source and lane test evidence.
- `Partial`: some baseline coverage exists, but one or more required JDBC-equivalent behaviors are missing or unproven.
- `Missing`: no lane implementation evidence found.

## MGA Recovery Contract

- This lane follows ScratchBird's MGA/state-based engine recovery model.
- Reconnect or reopen only repairs transport and session state.
- Reconnect never resurrects abandoned in-flight transactions or replay lost statements.
- Transaction recovery in the lane means reset, rollback, reopen, or retry against engine truth.
- Result resume is valid only for explicit suspended protocol states.
- `PrepareTransaction(...)`, `CommitPrepared(...)`, and
  `RollbackPrepared(...)` now expose explicit prepared / limbo control
  surfaces through canonical transaction-control SQL.
- `SupportsDormantReattach() -> false`, `DetachToDormant(...)`, and
  `ReattachDormant(...)` make dormant truth explicit and fail closed with
  `0A000` until the public front door exposes a real dormant token flow.
- native `READY`, `TXN_STATUS`, and `current_txn_id` are authoritative
  transaction-state surfaces; ScratchBird sessions stay always in a
  transaction and `COMMIT` / `ROLLBACK` immediately reopen the next boundary.
- `BeginTx(...)` still exposes only the standard `database/sql`
  isolation/read-only subset, but the driver-owned `BeginTxEx(...)` surface
  now exposes the canonical `READ COMMITTED` sub-mode selector directly.
- `BeginTx(...)` / `BeginTxEx(...)` restart the current default boundary with
  the requested settings, while still rejecting overlapping caller-owned `Tx`
  objects on the same connection.
- commit and rollback now drain the immediate reopen boundary before the next
  operation so the follow-on statement sees real result frames rather than a
  stray reopen `READY`.
- `CanonicalIsolationLabelForDriverIsolation(...)` makes the current alias
  mapping explicit in lane source: `READ UNCOMMITTED` remains a legacy
  compatibility alias, `READ COMMITTED` => canonical `READ COMMITTED`,
  `REPEATABLE READ` => canonical `SNAPSHOT`,
  `SERIALIZABLE` => canonical `SNAPSHOT TABLE STABILITY`.
- `CanonicalReadCommittedModeLabel(...)` makes the explicit
  `READ COMMITTED` sub-mode selector visible in lane source, including
  `READ COMMITTED READ CONSISTENCY`.
- `RetryScopeForSQLState(...)` makes the retry boundary explicit:
  `40001`/`40P01` => fresh statement only, `08xxx` => reconnect or reopen
  only, everything else => no automatic replay.
- the lane does not currently expose a standalone portal-resume API; any future
  resume surface must remain limited to explicit suspended protocol states.
- See `../../../../public_audit_summary`.

| GOBL group | JDBC baseline group(s) | Current status | Evidence anchors (lane source/tests) |
| --- | --- | --- | --- |
| `CONN` | `JDBCBL-CONN`, `JDBCBL-CFG` | `Implemented` | `config.go`, `auth_bootstrap.go`, `conn.go`, `protocol.go`, `scratchbird.go`, `config_test.go`, `conn_protocol_test.go`, `runtime_contract_gate_test.go` (protocol/SSL/compression normalization parity, `binary_transfer=false` compatibility, manager-proxy token guard, public staged `ProbeAuthSurface(...)` / connector probe coverage, resolved-auth reporting, auth-plugin startup parameter wiring, `SCRAM_SHA_512` + `TOKEN` execution, and fail-closed `MD5` / `PEER` / `REATTACH` coverage plus always-on manager-proxy runtime gate proof). |
| `TXN` | `JDBCBL-TXN` | `Implemented` | `conn.go`, `protocol.go`, `txn_exec_test.go`, `runtime_contract_gate_test.go`, `integration_test.go` (begin/commit/rollback plus savepoint/release/rollback-to APIs with wire validation, runtime-gate transaction lifecycle coverage, and live fresh-boundary adoption / post-rollback query proof). Standard `database/sql` entry points intentionally expose only the standard isolation/read-only subset, but the driver-owned `BeginTxEx(...)` plus `CanonicalReadCommittedModeLabel(...)` now expose the canonical `READ COMMITTED` sub-mode selector directly, compatible default fresh native boundaries are adopted instead of replaying a second `TXN_BEGIN`, `PrepareTransaction(...)` / `CommitPrepared(...)` / `RollbackPrepared(...)` make prepared / limbo control explicit, and no reconnect path is treated as transaction resurrection. |
| `EXEC` | `JDBCBL-EXEC` | `Implemented` | `conn.go`, `rows.go`, `rows_next_result_test.go`, `exec_surfaces.go`, `exec_surfaces_test.go`, `query_test.go`, `runtime_contract_gate_test.go` (simple/extended execution paths, multi-result traversal and summaries, callable/batch/generated-key APIs, and runtime gate multi-result execution coverage). |
| `META` | `JDBCBL-META` | `Implemented` | `metadata.go`, `metadata_rows.go`, `conn.go`, `metadata_test.go`, `config_test.go`, `runtime_contract_gate_test.go` (metadata collection resolver aliases, restriction-aware filtering semantics, schema expansion toggles, and runtime gate metadata query coverage). |
| `TYPE` | `JDBCBL-TYPE` | `Implemented` | `types.go`, `rows.go`, `types_test.go`, `runtime_contract_gate_test.go` (expanded OID encode/decode coverage including `timetz` and geometry families, OID metadata helper parity, and runtime gate binary decode coverage). |
| `ERR` | `JDBCBL-ERR` | `Implemented` | `errors.go`, `errors_test.go`, `errors_protocol_test.go`, `conn.go`, `conformance/harness.go` (SQLSTATE-to-kind mapping, retry-boundary classification (`statement` vs `reconnect` vs `none`), and protocol error translation with detail/hint propagation and truncation guards). |
| `RES` | `JDBCBL-RES` | `Implemented` | `conn.go`, `keepalive.go`, `leak_detector.go`, `circuit_breaker.go`, `telemetry.go`, `resilience_test.go`, `txn_exec_test.go` (connection lifecycle cleanup, `database/sql` `ResetSession` pool-handoff rollback/borrow-state clearing, dormant fail-closed surfaces, and keepalive/leak/circuit-breaker/telemetry resource guards with deterministic lane tests). |
