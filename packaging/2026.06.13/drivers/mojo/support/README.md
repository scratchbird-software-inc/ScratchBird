# ScratchBird Mojo Driver

Native ScratchBird driver lane for Mojo (SBWP v1.1).

Current implementation is a Mojo-Python interop lane:
- API/runtime shim in `src/scratchbird.py`
- Mojo entrypoints in `tests/*.mojo` invoke paired Python scripts for execution
  under the active Mojo toolchain

## Lane Docs

- [Baseline Requirement Mapping (S0)](BASELINE_REQUIREMENT_MAPPING.md)
- Getting started
- API reference
- [S2 TXN/EXEC Implementation](S2_TXN_EXEC_IMPLEMENTATION.md)
- [S3 Metadata Implementation](S3_METADATA_IMPLEMENTATION.md)
- [Tests](tests/README.md)

## MGA Recovery Contract

This lane follows ScratchBird's MGA/state-based engine recovery model.

- reconnect or reopen only repairs transport and session state
- reconnect never resurrects abandoned in-flight transactions or replay lost statements
- transaction recovery in the lane means reset, rollback, reopen, or retry against engine truth
- result resume is valid only for explicit suspended protocol states
- `begin(...)` exposes the lane's SQL-style compatibility aliases for
  transaction begin options, including `read_committed_mode`, and
  `canonical_isolation_label(...)` now makes the current mapping explicit in lane source:
  `READ UNCOMMITTED` remains a legacy compatibility alias,
  `READ COMMITTED` => canonical `READ COMMITTED`,
  `REPEATABLE READ` => canonical `SNAPSHOT`,
  `SERIALIZABLE` => canonical `SNAPSHOT TABLE STABILITY`
- `canonical_read_committed_mode_label(...)` now makes the canonical
  `READ COMMITTED` sub-mode selector explicit in lane source, including
  `READ COMMITTED READ CONSISTENCY`
- `retry_scope_for_sqlstate(...)` makes the retry boundary explicit:
  `40001`/`40P01` => fresh statement only, `08xxx` => reconnect or reopen
  only, everything else => no automatic replay
- on the live `sb_wire_transport=python` engine-endpoint path, native
  `READY`, `TXN_STATUS`, and `current_txn_id` own transaction-state truth;
  ScratchBird sessions stay always in a transaction and `COMMIT` /
  `ROLLBACK` reopen the next boundary
- `begin(...)` is documented against that always-in-transaction contract
  rather than idle-session semantics
- `commit()` and `rollback()` now preserve that reopened boundary and the
  live matrix proves the next post-rollback query sees the real result instead
  of a stale reopen frame
- `supports_prepared_transactions()` and
  `build_prepared_transaction_sql(...)` make prepared / limbo handling
  explicit in lane source, and `prepare_transaction(...)`,
  `commit_prepared(...)`, and `rollback_prepared(...)` use canonical
  transaction-control SQL rather than reconnect folklore
- `supports_dormant_reattach() -> false` plus fail-closed
  `detach_to_dormant(...)` / `reattach_dormant(...)` make dormant truth
  explicit in lane code
- this lane does not currently expose a standalone public portal-resume helper,
  and `supports_portal_resume() -> false` keeps that absence explicit instead
  of implying reconnect-based continuation

See `../../../../public_audit_summary`.

## Status

- Full SBWP v1.1 API surface is represented in-lane through the Python-backed shim.
- Public staged auth/bootstrap is now exposed on the shim surface through `probe_auth_surface(...)`, `AuthMethodSurface`, `AuthProbeResult`, and `ResolvedAuthContext`, with per-connection `get_resolved_auth_context()` reporting on both deterministic and `sb_wire_transport=python` paths.
- Runtime now includes an opt-in wire-capable bridge path in `src/scratchbird.py` (`sb_wire_transport=python` / `SCRATCHBIRD_MOJO_WIRE_TRANSPORT`) via `_PythonWireConnection`, `_WireStatement`, and `_WireStream`, with deterministic fallback preserved.
- Mojo wrappers and test adapter now execute under pixi-managed Mojo toolchains.
- `src/scratchbird.mojo` now compiles in current Mojo syntax as a facade over `src/scratchbird_native.mojo`, with deterministic facade smoke in `tests/scratchbird_surface.mojo`.
- Native bootstrap module in current Mojo syntax is available at `src/scratchbird_native.mojo` and validated by `tests/native_bootstrap.mojo`.
- Native bootstrap currently covers deterministic connect/ping guards, extended metadata alias/query resolution, transaction lifecycle guards (`25001` nested begin), savepoint lifecycle guards (`25000`/`3B001`), prepare-bind mismatch handling, prepared execute parity (including statement-close `HY010` guard behavior), paging-query rowcount semantics, and stream/cancel (`57014`) with post-cancel recovery semantics.
- Native bootstrap DSN parsing now includes transport-ready credentials/endpoint fields (`user`/`password`, `host`, `port`), query override and alias support (`user|username|pguser`, `password|passwd|pgpassword`, `host|hostname|servername|pghost`, `port|portNumber|pgport`, `database|dbname|databaseName|pgdatabase`) with query-order alias precedence (last matching key wins across each family), JDBC-style session knobs (`role`, `application_name|applicationname`, `autocommit|auto_commit`, `readonly|read_only`, `current_schema|search_path|searchPath|currentschema`, `default_row_fetch_size|fetch_size|fetchSize|defaultrowfetchsize`, `metadata_expand_schema_parents|metadataexpandschemaparents|expand_schema_parents|expandschemaparents|dbeaver_expand_schema_parents|dbeaverexpandschemaparents`) with query-order alias precedence (including `autocommit|auto_commit` and `readonly|read_only`), JDBC statement/logging knobs (`prepare_threshold|preparethreshold`, `rewrite_batched_inserts|rewritebatchedinserts`, `logger_level|loggerlevel|log_level|loglevel`, `logger_file|loggerfile|log_file|logfile`) with query-order alias precedence, TLS material knobs (`sslrootcert`, `sslcert`, `sslkey`, `sslpassword`, and underscore aliases) with query-order alias precedence (`ssl_root_cert|sslrootcert`, `ssl_cert|sslcert`, `ssl_key|sslkey`, `ssl_password|sslpassword`), pooling knobs (`tcpkeepalive`, `pooling`, `min_pool_size|minpoolsize`, `max_pool_size|maxpoolsize`, `connection_lifetime|connectionlifetime|poolingconnectionlifetime`), manager knobs (`manager_*|mcp_*`, including defaults `manager_connection_profile=SBsql`, `manager_client_intent=SBsql`, `manager_auth_fast_path=true`) with query-order alias precedence (including `manager_auth_token|mcp_auth_token`), bracketed IPv6 host parsing (`[::1]:3092`) plus malformed bracketed-IPv6 guard handling, timeout aliases (`connect_timeout|connecttimeout`, `socket_timeout|sockettimeout`, `login_timeout|logintimeout`, `acquire_timeout|acquiretimeout` with fallback `pooling_acquire_timeout|poolingacquiretimeout`), protocol aliases (`protocol|parser|dialect` with canonicalization of `scratchbird`, `scratchbird-native`, `scratchbird_native` to `native` and query-order alias precedence), front-door mode normalization (`manager-proxy`/`managerproxy`/`managed` → `manager_proxy`) with query-order precedence across `front_door_mode|frontdoormode|connection_mode|ingress_mode` (last matching key wins) and invalid-value `0A000` parity guards, transport alias support (`binary_transfer|binarytransfer`, including `binary_transfer=false` compatibility), `ssl` alias support for `sslmode` (including accepted `disable`), compression normalization/validation (`none` → `off`; `zstd` accepted; unsupported values such as `gzip` rejected) with repeated-key last-value precedence, URL-style query decoding for DSN values (`%xx`, `+`), endpoint/timeout/session/pooling/manager guards (host defaults to `localhost` when omitted, explicit empty host rejected, `port` in `1..65535`, timeout values `>= 0`, `default_row_fetch_size >= 0`, `min_pool_size >= 0`, `max_pool_size >= 1`, `min_pool_size <= max_pool_size`, `connection_lifetime >= 0`, `manager_client_flags >= 0`, malformed integer DSN values rejected with deterministic `22023` errors including trailing-alias malformed-value rejection for `port|portNumber|pgport`, `default_row_fetch_size|fetch_size|fetchSize|defaultrowfetchsize`, `prepare_threshold|preparethreshold`, `connection_lifetime|connectionlifetime|poolingconnectionlifetime`, and `manager_client_flags|mcp_client_flags`, manager token required for manager-proxy via `08001`, malformed percent escapes rejected with `22023`), and deterministic auth-fail guard parity via `sb_test_auth_fail=true` (`28P01`) in native/facade smoke coverage.
- Native bootstrap now enforces closed-connection operation guards (`08003`) across query/begin/commit/rollback/cancel/stream/metadata paths, deterministic `ping()` behavior after close (`false`), and deterministic integer-parameter coercion guards (`22023`) for parameterized integer query/prepare execution.
- Stream lifecycle parity now includes deterministic closed-stream read guard behavior (`HY010`) and active-stream read guards against closed connections (`08003`).
- Native bootstrap connection identity now includes endpoint context (`user@host:port/database`) and is asserted in both native/facade smoke lanes for deterministic lifecycle tracking hooks.
- Native bootstrap guard and unsupported-operation failures now use deterministic SQLSTATE-prefixed error strings with extractor coverage (`extract_sqlstate`) in lane tests.
- Metadata execution parity now includes deterministic metadata restriction helpers (`normalize_metadata_restriction_key`, `resolve_metadata_collection_query_restricted`, `resolve_metadata_collection_query_restricted_multi`, `query_metadata_restricted`, `query_metadata_rows_restricted`, `query_metadata_restricted_multi`, `query_metadata_rows_restricted_multi`) with expanded alias keys (`catalog`/`index`/`constraint`/`routine`/`type`) plus collapsed/camel restriction aliases (for example `tableSchem`, `tableCatalog`, `dataTypeName`), duplicate-alias precedence semantics (`last matching key wins`, including empty-value clear behavior), cross-collection schema/table/index/constraint/routine/type restriction predicates, exact/wildcard/null restriction shaping (`=`, `LIKE ... ESCAPE '\\'`, `IS NULL`) including escaped wildcard patterns, SQLSTATE guards for invalid restriction payloads (`07001` native count mismatch / `22023` shim non-mapping restrictions), deterministic DDL-editor payload shaping helpers (`build_ddl_editor_schema_payload`, `ddl_editor_schema_payload`), and executable rowcount coverage in shim/native bootstrap/facade scaffolds.
- Integration smoke now exercises transaction/savepoint lifecycle and prepare/stream-cancel recovery checks in-lane, plus metadata stability/payload checks with schemas/tables/columns rowcount relationship invariants, alias-family restriction execution checks, `ddl_editor_schema_payload(...)` contract/tree-parent checks, deterministic fallback content assertions for schema restriction/payload shaping, long-running stream cancel checks, lifecycle snapshot checks, and direct/manager/listener DSN matrices (`SCRATCHBIRD_MOJO_*_URLS`).
- Native bootstrap query/stream paths now exercise circuit-breaker/keepalive/telemetry hooks plus leak-detector/pipeline lifecycle scaffolds (deterministic integration), including deterministic SQLSTATE guards for pipeline-capacity (`54000`) and circuit-breaker-open (`08006`) behavior, auto-vs-manual pipeline flush semantics, and half-open breaker recovery checks.
- Integration and conformance launchers are native-smoke-first (`tests/scratchbird_surface.mojo` then `tests/native_bootstrap.mojo`) with bridge-shim fallback controls, matrix DSN support, and optional live-wire routing.
- Bridge-shim connection parity now includes `prepare`/statement execute lifecycle guards (`HY010` closed statement, `08003` prepare-on-closed-connection), deterministic integer-parameter coercion guards (`22023`), deterministic `ping`, transaction/savepoint helpers, begin-option integer validation parity for `conflict_action|autocommit_mode|isolation_level|access_mode|deferrable|wait_mode|wait|timeout_ms` (`22023`), wire transaction state transitions (`_txn_id` + savepoint reset on begin/commit/rollback), static/wire begin-option state persistence/clear semantics (`_txn_begin_options` on begin, clear on commit/rollback), static/wire closed-connection guards (`08003`) across begin/commit/rollback/savepoint/query helpers (including metadata rowcount/restriction helpers, `get_schema`, and `ddl_editor_schema_payload` routes), metadata helper guard precedence so closed connections return `08003` before unsupported collection/restriction validation, shared metadata rowcount fallback semantics for both static and instance helpers (`rowcount` non-negative integer else `len(rows)` else `0`, including unsized-row fallback plus boolean/negative-rowcount rejection), shared metadata row extraction fallback semantics for `get_schema` and `ddl_editor_schema_payload` restriction routes (`rows` normalization to list/tuple only; mapping/text/unsupported-iterable rows fallback to `[]`), deterministic static savepoint tracking initialization for missing/non-list `_savepoints`, and closed-connection operation guards (`08003`) across query/begin/commit/rollback/cancel/stream/metadata used by lane tests; connect-guard parity now also includes query-order front-door alias normalization/token enforcement (`front_door_mode|frontdoormode|connection_mode|ingress_mode`, last matching key wins, `08001` token guard), compatibility for `binary_transfer=false` / `binarytransfer=false` and `compression=zstd|none`, TLS-required `sslmode|ssl=disable` rejection plus invalid front-door/unsupported compression SQLSTATE parity (`0A000`), query-order manager token alias precedence (`manager_auth_token|mcp_auth_token`), malformed query-escape and malformed bracketed-IPv6 DSN guards (`22023`), native-only protocol alias normalization/rejection for `protocol|parser|dialect` (`0A000`), explicit `user/database` required and empty-host endpoint guards (`28000`), port validity/range guards (`22023`) including trailing-alias malformed-value rejection (`port|portNumber|pgport`), timeout alias guards (`connect_timeout|connecttimeout`, `socket_timeout|sockettimeout`, `login_timeout|logintimeout`, `acquire_timeout|acquiretimeout`, fallback `pooling_acquire_timeout|poolingacquiretimeout` with `>=0` enforcement) with query-order last-matching-alias precedence and malformed-trailing-alias rejection, and extended pooling/session/lifecycle integer guards (`prepare_threshold`, `default_row_fetch_size`, `min_pool_size`, `max_pool_size`, `connection_lifetime`, `manager_client_flags`, `cb_failure_threshold`, `cb_recovery_timeout_ms`, `cb_success_threshold`, `cb_half_open_max_requests`, `keepalive_max_idle_before_check_ms`, `leak_threshold_ms`, `pipeline_max_in_flight`, `pipeline_auto_flush_threshold`) with deterministic `22023` validation (including trailing-alias malformed-value rejection for `default_row_fetch_size|fetch_size|fetchSize|defaultrowfetchsize`).
- Bridge-shim type codecs now include temporal/json/jsonb/uuid wrappers and array-of-composite encode/decode coverage for deterministic lane testing.
- Lifecycle scaffolds (`circuit_breaker`/`leak_detector`/`keepalive`/`telemetry`/`pipeline`) now compile in current Mojo syntax and have dedicated deterministic smoke coverage in `tests/lifecycle_scaffolds.mojo`.
- Native Mojo transport/auth remains future work.

## Platform Support

| Platform | Status | Notes |
|----------|--------|-------|
| Linux | Experimental | Validated with pixi-managed Mojo toolchain. |
| Windows | Not supported | No CI/toolchain path configured. |
| macOS | Not supported | No CI/toolchain path configured. |

## Requirements

- Python 3.10+
- Mojo toolchain (recommended: `pixi` workspace at `~/mojo-work/sb-mojo`)

## Verification

From `lanes/active/drivers/mojo`:

```bash
pixi run -m ~/mojo-work/sb-mojo --executable mojo run -O0 -j1 -I src -I src/scratchbird tests/native_bootstrap.mojo
pixi run -m ~/mojo-work/sb-mojo --executable mojo run -O0 -j1 -I src -I src/scratchbird tests/scratchbird_surface.mojo
pixi run -m ~/mojo-work/sb-mojo --executable mojo run -O0 -j1 tests/auth_bootstrap_contract.mojo
pixi run -m ~/mojo-work/sb-mojo --executable mojo run -O0 -j1 tests/metadata_recursive_schema.mojo
pixi run -m ~/mojo-work/sb-mojo --executable mojo run -O0 -j1 tests/txn_exec_parity.mojo
pixi run -m ~/mojo-work/sb-mojo --executable mojo run -O0 -j1 tests/errors.mojo
pixi run -m ~/mojo-work/sb-mojo --executable mojo run -O0 -j1 tests/type_codecs.mojo
pixi run -m ~/mojo-work/sb-mojo --executable mojo run -O0 -j1 tests/connection_guards.mojo
pixi run -m ~/mojo-work/sb-mojo --executable mojo run -O0 -j1 -I src/scratchbird tests/lifecycle_scaffolds.mojo
python3 tests/wire_transport_bridge.py
python3 tests/auth_bootstrap_contract.py
python3 tests/integration.py
python3 tests/sbdriver_conformance.py --manifest ../../../../docs/fixtures/sbwp_conformance_manifest.json
tests/sbdriver-conformance --manifest ../../../../docs/fixtures/sbwp_conformance_manifest.json
```

CI (`.github/workflows/ci.yml`, Mojo gated lane) now runs an explicit sequence:
`scratchbird_surface.mojo`, `native_bootstrap.mojo`, `metadata_execution.mojo`,
`metadata_recursive_schema.mojo`, `wire_transport_bridge.py`, `integration.py`, and `sbdriver-conformance`,
with optional live matrix execution when `MOJO_LIVE_MATRIX_ENABLED=true`.

Optional launcher env vars:
- `SCRATCHBIRD_MOJO_URL` for direct smoke
- `SCRATCHBIRD_MOJO_DIRECT_URLS` for direct matrix smoke (comma/newline separated)
- `SCRATCHBIRD_MOJO_MANAGER_URL` for manager-proxy smoke
- `SCRATCHBIRD_MOJO_MANAGER_URLS` for manager-proxy matrix smoke (comma/newline separated)
- `SCRATCHBIRD_MOJO_LISTENER_URL` for listener smoke
- `SCRATCHBIRD_MOJO_LISTENER_URLS` for listener matrix smoke (comma/newline separated)
- `SCRATCHBIRD_MOJO_BAD_AUTH_URL` for bad-auth smoke (shim-mode deterministic path can append `sb_test_auth_fail=true`)
- `SCRATCHBIRD_MOJO_BAD_AUTH_URLS` for bad-auth matrix smoke (comma/newline separated)
- `SCRATCHBIRD_MOJO_WIRE_TRANSPORT` to opt-in wire bridge routing (`python`) for non-deterministic DSNs
- `SCRATCHBIRD_MOJO_NATIVE_RUN_ARGS` to override native smoke compiler flags (default `-O0 -j1`)
- `SCRATCHBIRD_MOJO_SKIP_NATIVE_BOOTSTRAP` to bypass native smoke (`tests/scratchbird_surface.mojo` and `tests/native_bootstrap.mojo`) in `tests/integration.py` and `tests/sbdriver_conformance.py`
- `SCRATCHBIRD_MOJO_NATIVE_REQUIRED` to fail when native bootstrap launcher is unavailable/failing
- `SCRATCHBIRD_MOJO_DISABLE_FALLBACK_DSN` to require explicit direct/manager/listener/bad-auth DSN env configuration for integration/conformance (default lane behavior uses deterministic fallback DSNs)

Deterministic native lifecycle DSN knobs (for lane smoke/testing):
- `cb_failure_threshold`
- `cb_recovery_timeout_ms`
- `cb_success_threshold`
- `cb_half_open_max_requests`
- `keepalive_max_idle_before_check_ms`
- `leak_threshold_ms`
- `pipeline_max_in_flight`
- `pipeline_auto_flush`
- `pipeline_auto_flush_threshold`

## Next Steps

- Replace the Python transport bridge with native Mojo sockets/TLS
- Add Mojo-native streaming helpers and type wrappers
