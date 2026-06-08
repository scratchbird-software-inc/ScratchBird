# ScratchBird Driver Baseline Requirement Mapping (ELIXIRBL -> JDBC Baseline)

Scope: `lanes/active/drivers/elixir` lane only.

Status legend:
- `Implemented`: baseline behavior is present and anchored by lane source/tests.
- `Partial`: baseline behavior exists but has explicit scope limits or incomplete validation coverage.

## MGA Recovery Contract

- This lane follows ScratchBird's MGA/state-based engine recovery model.
- Reconnect or reopen only repairs transport and session state.
- Reconnect never resurrects abandoned in-flight transactions or replay lost statements.
- Transaction recovery in the lane means reset, rollback, reopen, or retry against engine truth.
- Result resume is valid only for explicit suspended protocol states.
- Native `READY`, `TXN_STATUS`, and `current_txn_id` are authoritative for
  transaction activity; ScratchBird sessions stay always in a transaction and
  `COMMIT` / `ROLLBACK` reopen the next boundary.
- `begin/2` is documented against that always-in-transaction contract rather
  than idle-session semantics.
- The focused live recovery slice in `test/integration_test.exs` now proves
  that rollback leaves the next query immediately usable on the reopened
  native boundary with no reconnect and no statement replay.
- `begin/2` documents the alias mapping
  `:read_committed` => canonical `READ COMMITTED`,
  `:repeatable_read` => canonical `SNAPSHOT`,
  `:serializable` => canonical `SNAPSHOT TABLE STABILITY`.
- `begin/2` now also exposes the canonical `READ COMMITTED` sub-mode selector
  directly through `:read_committed_mode`, including
  `READ COMMITTED READ CONSISTENCY`.
- `ScratchBird.canonical_read_committed_mode_label/1` keeps that selector
  source-visible for auditors and lane tests.
- `ScratchBird.retry_scope/1` makes the retry boundary explicit:
  `40001`/`40P01` => statement only, `08xxx` => reconnect only, all other
  SQLSTATEs => no automatic replay.
- Prepared / limbo truth is explicit in lane source through
  `supports_prepared_transactions/0`, `build_prepared_transaction_sql/2`,
  `prepare_transaction/2`, `commit_prepared/2`, and `rollback_prepared/2`,
  which emit canonical transaction-control SQL.
- Dormant truth is explicit in lane source through
  `supports_dormant_reattach/0`, `detach_to_dormant/1`, and
  `reattach_dormant/3`, which all fail closed with `0A000`.
- This lane does not expose a standalone public portal-resume helper;
  `supports_portal_resume/0 -> false` keeps that boundary source-visible
  instead of implying reconnect-based continuation.
- See `../../../../public_audit_summary`.

| ELIXIRBL group | JDBC baseline group | Current status | Evidence anchors (lane source/tests) | Notes |
| --- | --- | --- | --- | --- |
| `CONN` | `JDBCBL-CONN` | `Implemented` | `lib/scratchbird/auth_bootstrap.ex`; `lib/scratchbird/config.ex`; `lib/scratchbird/connection.ex`; `lib/scratchbird/protocol.ex`; `lib/scratchbird/scram.ex`; `lib/scratchbird.ex`; `test/scratchbird_test.exs`; `test/connection_validation_test.exs`; `test/auth_bootstrap_contract_test.exs`; `test/integration_test.exs` | DSN parsing, protocol/front-door validation, staged `probe_auth_surface/1`, resolved-auth reporting via `get_resolved_auth_context/1`, direct and manager-proxy ingress detection, executable `PASSWORD`/`SCRAM_SHA_256`/`SCRAM_SHA_512`/`TOKEN`, fail-closed `MD5`/`PEER`/`REATTACH`, and env-gated live native recovery proof are present in-lane. |
| `TXN` | `JDBCBL-TXN` | `Implemented` | `lib/scratchbird/connection.ex`; `lib/scratchbird/protocol.ex`; `lib/scratchbird.ex`; `test/txn_begin_test.exs`; `test/integration_test.exs` | Begin/commit/rollback/savepoint APIs and payload builders exist, the lane exposes the canonical `READ COMMITTED` sub-mode selector through `:read_committed_mode`, default `begin/2` now adopts compatible fresh native boundaries instead of forcing a redundant `TXN_BEGIN`, and prepared / dormant capability truth is explicit with focused parity and live recovery proof. |
| `EXEC` | `JDBCBL-EXEC` | `Partial` | `lib/scratchbird/connection.ex`; `lib/scratchbird/protocol.ex`; `test/txn_begin_test.exs`; `test/integration_test.exs`; `test/ecto_adapter_test.exs` | Query/extended execution and Ecto SQL shaping are present. This lane does not expose a standalone public portal-resume helper; `supports_portal_resume/0 -> false` makes that boundary explicit, but deterministic stream/paging proof is still limited. |
| `META` | `JDBCBL-META` | `Implemented` | `lib/scratchbird/metadata.ex`; `lib/scratchbird.ex`; `test/metadata_test.exs` | Required sys catalog metadata query surfaces are present with direct lane tests, including routines, catalogs, primary/foreign keys, table/column privileges, and type info. Remaining gap is live metadata proof, not local query-family breadth. |
| `TYPE` | `JDBCBL-TYPE` | `Implemented` | `lib/scratchbird/types.ex`; `test/types_test.exs` | Scalar/array/vector/range/composite/network type encode-decode coverage is present in dedicated lane tests. |
| `ERR` | `JDBCBL-ERR` | `Implemented` | `lib/scratchbird/errors.ex`; `test/errors_test.exs` | SQLSTATE class mapping, explicit retry-boundary helpers, and fallback error behavior are covered by dedicated lane tests. |
| `RES` | `JDBCBL-RES` | `Partial` | `lib/scratchbird/connection.ex`; `lib/scratchbird_ecto/connection.ex`; `lib/scratchbird/circuit_breaker.ex`; `lib/scratchbird/keepalive.ex`; `lib/scratchbird/leak_detector.ex`; `lib/scratchbird/telemetry.ex`; `test/ecto_adapter_test.exs`; `test/integration_test.exs` | Resilience primitives are present. This lane currently uses fresh-connect-only recovery rather than transparent in-place reconnect; `disconnect/2` tears down the current wire/session and a replacement session must come from a new `connect/1` handshake, preventing abandoned local transaction state from being reused across the disconnect boundary. The focused live native slice now proves rollback leaves the next query immediately usable on the reopened fresh boundary. |
