# ScratchBird Driver Baseline Requirement Mapping (S0)

## Scope

- Lane-local S0 artifact only for `lanes/active/drivers/mojo`.
- Evidence is restricted to files in this lane; no cross-lane claims are made.

## MGA Recovery Contract

- This lane follows ScratchBird's MGA/state-based engine recovery model.
- Reconnect or reopen only repairs transport and session state.
- Reconnect never resurrects abandoned in-flight transactions or replay lost statements.
- Transaction recovery in the lane means reset, rollback, reopen, or retry against engine truth.
- Result resume is valid only for explicit suspended protocol states.
- `begin(...)` exposes the lane's SQL-style compatibility aliases for
  transaction begin options, including `read_committed_mode`.
- `canonical_isolation_label(...)` now makes the current isolation alias
  mapping explicit in lane source: `READ UNCOMMITTED` remains a legacy
  compatibility alias, `READ COMMITTED` => canonical `READ COMMITTED`,
  `REPEATABLE READ` => canonical `SNAPSHOT`,
  `SERIALIZABLE` => canonical `SNAPSHOT TABLE STABILITY`.
- `canonical_read_committed_mode_label(...)` now makes the canonical
  `READ COMMITTED` sub-mode selector explicit in lane source, including
  `READ COMMITTED READ CONSISTENCY`.
- `retry_scope_for_sqlstate(...)` makes the retry boundary explicit:
  `40001`/`40P01` => fresh statement only, `08xxx` => reconnect or reopen
  only, everything else => no automatic replay.
- `supports_prepared_transactions()` and
  `build_prepared_transaction_sql(...)` make prepared / limbo handling
  explicit in lane source, and `prepare_transaction(...)`,
  `commit_prepared(...)`, and `rollback_prepared(...)` use canonical
  transaction-control SQL rather than reconnect folklore.
- `supports_dormant_reattach() -> false` plus fail-closed
  `detach_to_dormant(...)` / `reattach_dormant(...)` make dormant truth
  explicit in lane code.
- This lane does not currently expose a standalone public portal-resume API,
  and `supports_portal_resume() -> false` keeps that absence explicit instead
  of implying reconnect-based continuation.
- See `../../../../public_audit_summary`.

## CONN (JDBCBL)

- Current status: Implemented (current-syntax native facade/bootstrap + opt-in SBWP wire bridge runtime)
- Lane-local source anchors:
- `src/scratchbird.mojo:12` (`scratchbird` current-syntax facade exports `ScratchBirdConfig`/`ScratchBirdConnection`/`connect` from native bootstrap)
- `src/scratchbird.mojo:20` (facade exports native `validate_connect_guards`)
- `src/scratchbird_native.mojo:28` (native-bootstrap `ScratchBirdConfig` DSN parsing surface, including credential extraction/overrides and alias keys)
- `src/scratchbird_native.mojo:88` (native-bootstrap `user|username|pguser` / `password|passwd|pgpassword` query overrides layered over DSN userinfo parsing with query-order alias precedence (last matching key wins))
- `src/scratchbird_native.mojo:101` (native-bootstrap `host|hostname|servername|pghost`, `port|portNumber|pgport`, and `database|dbname|databaseName|pgdatabase` query overrides layered over DSN endpoint/path parsing, including bracketed IPv6 hosts and omitted-host defaulting to `localhost`, with query-order alias precedence (last matching key wins))
- `src/scratchbird_native.mojo:114` (native-bootstrap session property parsing: `role`, `application_name|applicationname`, `autocommit|auto_commit`, `readonly|read_only`, `current_schema|search_path|searchpath|currentschema` with default `public`, `default_row_fetch_size|fetch_size|fetchsize|defaultrowfetchsize` with query-order alias precedence)
- `src/scratchbird_native.mojo:124` (native-bootstrap metadata/session alias parsing for `metadata_expand_schema_parents|metadataexpandschemaparents|expand_schema_parents|expandschemaparents|dbeaver_expand_schema_parents|dbeaverexpandschemaparents`)
- `src/scratchbird_native.mojo:148` (native-bootstrap JDBC parity property parsing: `prepare_threshold|preparethreshold`, `rewrite_batched_inserts|rewritebatchedinserts`, `logger_level|loggerlevel|log_level|loglevel`, `logger_file|loggerfile|log_file|logfile`, with query-order alias precedence)
- `src/scratchbird_native.mojo:260` (native-bootstrap TLS property parsing: `sslmode|ssl`, `sslrootcert`, `sslcert`, `sslkey`, `sslpassword` and underscore aliases, with query-order alias precedence for `sslmode|ssl` and TLS material alias pairs `ssl_root_cert|sslrootcert`, `ssl_cert|sslcert`, `ssl_key|sslkey`, `ssl_password|sslpassword`)
- `src/scratchbird_native.mojo:169` (native-bootstrap pooling parsing: `tcpkeepalive`, `pooling`, `min_pool_size|minpoolsize`, `max_pool_size|maxpoolsize`, `connection_lifetime|connectionlifetime|poolingconnectionlifetime`, with query-order alias precedence for integer alias families)
- `src/scratchbird_native.mojo:196` (native-bootstrap manager parsing: `manager_*|mcp_*` aliases and defaults for `manager_connection_profile`, `manager_client_intent`, and `manager_auth_fast_path`, with query-order alias precedence including `manager_auth_token|mcp_auth_token`)
- `src/scratchbird_native.mojo:283` (native-bootstrap connection identity formatting includes endpoint context: `user@host:port/database`)
- `src/scratchbird_native.mojo:209` (native-bootstrap front-door mode normalization with query-order alias resolution across `front_door_mode|frontdoormode|connection_mode|ingress_mode` (last matching key wins), including `managed` normalization)
- `src/scratchbird_native.mojo:244` (native-bootstrap protocol selector parsing: `protocol|parser|dialect` with query-order alias precedence (last matching key wins))
- `src/scratchbird_native.mojo:1167` (native-bootstrap protocol canonicalization maps `scratchbird` / `scratchbird-native` / `scratchbird_native` to `native`)
- `src/scratchbird_native.mojo:225` (native-bootstrap timeout alias parsing: `connect_timeout|connecttimeout`, `socket_timeout|sockettimeout`, `login_timeout|logintimeout`, `acquire_timeout|acquiretimeout` with fallback `pooling_acquire_timeout|poolingacquiretimeout`)
- `src/scratchbird_native.mojo:287` (native-bootstrap compression normalization/compatibility: `none` -> `off`, `zstd` accepted, repeated `compression` keys resolved by query-order last-key precedence)
- `src/scratchbird_native.mojo:822` (native-bootstrap malformed bracketed-IPv6 authority detection helper)
- `src/scratchbird_native.mojo:893` (native-bootstrap DSN query value decoding for `%xx` and `+`)
- `src/scratchbird_native.mojo:938` (native-bootstrap malformed query-escape detection helper)
- `src/scratchbird_native.mojo:1140` (native-bootstrap query-order DSN integer helpers include last-matching alias resolution + malformed-any-alias detection across endpoint/session/pooling/timeout alias families)
- `src/scratchbird_native.mojo:229` (native-bootstrap lifecycle DSN knob parsing: `cb_*` / `keepalive_*` / `leak_*` / `pipeline_*`)
- `src/scratchbird_native.mojo:1658` (native-bootstrap connect guards: malformed query-escape guard (`22023`), malformed bracketed-IPv6 guard (`22023`), strict malformed-integer DSN guards (`22023`) including query-order trailing-alias malformed-value rejection for `port|portNumber|pgport`, `default_row_fetch_size|fetch_size|fetchSize|defaultrowfetchsize`, `prepare_threshold|preparethreshold`, `connection_lifetime|connectionlifetime|poolingconnectionlifetime`, `manager_client_flags|mcp_client_flags`, timeout aliases (`connect/socket/login/acquire`, plus `pooling_acquire_timeout`), and other integer alias families, plus protocol/compression/front-door/user+db + endpoint/timeout/session/pooling/manager guardrails (including invalid `front_door_mode` `0A000`, manager-proxy token requirement `08001`, explicit empty-host rejection, non-negative guards for `default_row_fetch_size`, `min_pool_size`, `connection_lifetime`, `manager_client_flags`, plus `max_pool_size >= 1` and `min_pool_size <= max_pool_size`); accepts `sslmode=disable`, `binary_transfer=false`, and rejects unsupported compression values outside `off|zstd`)
- `src/scratchbird_native.mojo:1569` (native-bootstrap `connect` entrypoint)
- `src/scratchbird_native.mojo:453` (native-bootstrap `ping` surface)
- `src/scratchbird_native.mojo:535` (native-bootstrap closed-connection operation guard helper returns SQLSTATE `08003` across query/begin/commit/rollback/cancel/stream/metadata routes)
- `src/scratchbird.py:693` (bridge-shim connect guard enforcement with query-order alias resolution helpers and deterministic auth-fail simulation)
- `src/scratchbird.py:251` (bridge-shim public staged auth/bootstrap surface types: `AuthMethodSurface`, `AuthProbeResult`, and `ResolvedAuthContext`)
- `src/scratchbird.py:1110` (bridge-shim shared target-resolution guard helper `_resolve_connect_target(...)` lets staged probe validate DSN/front-door/protocol/endpoint truth without requiring identity or manager token too early)
- `src/scratchbird.py:1357` (bridge-shim deterministic staged probe infers truthful direct/manager auth requirements, including fail-closed non-local methods such as `PEER`)
- `src/scratchbird.py:1431` (bridge-shim public `probe_auth_surface(...)` delegates to the upgraded Python lane on `sb_wire_transport=python` and otherwise returns deterministic staged auth/bootstrap truth)
- `src/scratchbird.py:726` (bridge-shim malformed query-escape and malformed bracketed-IPv6 guards (`22023`) plus protocol alias normalization/rejection (`protocol|parser|dialect` -> native-only `0A000`))
- `src/scratchbird.py:801` (bridge-shim front-door normalization/token enforcement (`08001`) with query-order alias precedence (`front_door_mode|frontdoormode|connection_mode|ingress_mode`) and manager token aliases (`manager_auth_token|mcp_auth_token`), binary/compression compatibility (`binary_transfer=false`, `compression=zstd|none`), TLS-required `sslmode|ssl=disable` rejection and invalid front-door/unsupported compression SQLSTATE parity (`0A000`), `user/database` required and explicit empty-host guards (`28000`), port validity/range guards (`22023`), timeout alias guards (`connect_timeout|connecttimeout`, `socket_timeout|sockettimeout`, `login_timeout|logintimeout`, `acquire_timeout|acquiretimeout`, fallback `pooling_acquire_timeout|poolingacquiretimeout` with `>=0` enforcement), and extended integer guards (`prepare_threshold`, `default_row_fetch_size`, `min_pool_size`, `max_pool_size`, `connection_lifetime`, `manager_client_flags`, `cb_failure_threshold`, `cb_recovery_timeout_ms`, `cb_success_threshold`, `cb_half_open_max_requests`, `keepalive_max_idle_before_check_ms`, `leak_threshold_ms`, `pipeline_max_in_flight`, `pipeline_auto_flush_threshold`) with deterministic `22023` validation)
- `src/scratchbird.py:1264` (bridge-shim `connect` entrypoint)
- `src/scratchbird.py:360` (bridge transport-mode selector for deterministic vs wire runtime via `sb_wire_transport` / `SCRATCHBIRD_MOJO_WIRE_TRANSPORT`)
- `src/scratchbird.py:1554` (wire-capable runtime adapter `_PythonWireConnection` for query/prepare/stream/cancel/txn/metadata routing)
- `src/scratchbird.py:1879` (bridge-shim deterministic connections expose `get_resolved_auth_context()` with attached/closed truth)
- `src/scratchbird.py:2132` (wire-capable runtime adapter exposes `get_resolved_auth_context()` and mirrors delegated auth/bootstrap truth from the upgraded Python lane)
- `src/scratchbird.py:2129` (bridge `connect` runtime selector that dispatches deterministic shim or wire adapter)
- Lane-local test anchors:
- `tests/scratchbird_surface.mojo:181` (current-syntax `scratchbird` facade connect/ping/query smoke + DSN credential/host/port/database alias parse assertions (including JDBC/PG aliases), query-order alias precedence checks for endpoint/credential and manager token aliases, protocol alias canonicalization checks, and mode normalization/precedence checks)
- `tests/auth_bootstrap_contract.py:81` (bridge-shim staged auth/bootstrap contract proof for deterministic direct/password, deterministic fail-closed `PEER`, manager/TOKEN ingress probe without manager token, and `sb_wire_transport=python` delegation to Python-lane staged probe + resolved-auth context)
- `tests/auth_bootstrap_contract.mojo:1` (Mojo launcher wrapper for the staged auth/bootstrap contract script)
- `tests/scratchbird_surface.mojo:75` (facade helper asserts session+metadata alias parsing (including `currentSchema`/`defaultRowFetchSize` plus `autocommit|auto_commit` and `readonly|read_only` query-order precedence), prepare-threshold/batched-insert/logger alias parsing, TLS material alias/default parsing with query-order precedence (`ssl_root_cert|sslrootcert`, `ssl_cert|sslcert`, `ssl_key|sslkey`, `ssl_password|sslpassword`), pooling/manager alias parsing, `ssl` alias parsing, accepted `binarytransfer=false` and `compression=zstd` connectivity, compression normalization including repeated-key last-value precedence, URL-style query decoding, and timeout-alias query-order last-match precedence)
- `tests/scratchbird_surface.mojo:173` (facade smoke asserts bracketed IPv6 host/port parsing)
- `tests/scratchbird_surface.mojo:326` (facade smoke asserts deterministic `connection_id` endpoint formatting)
- `tests/scratchbird_surface.mojo:710` (facade deterministic auth-fail guard SQLSTATE assertion via `sb_test_auth_fail=true`)
- `tests/scratchbird_surface.mojo:666` (facade guard SQLSTATE assertions for transport/protocol/front-door/compression/malformed-query aliases and malformed bracketed-IPv6 + malformed-integer DSN guards, plus negative timeout/session/pooling/manager DSNs, including manager-proxy token requirement, explicit empty-host rejection, and `default_row_fetch_size`/`min_pool_size`/`max_pool_size`/`connection_lifetime`/`manager_client_flags` guards)
- `tests/scratchbird_surface.mojo:910` (facade guard assertions that malformed trailing aliases still fail deterministically with SQLSTATE `22023` across timeout, port, `default_row_fetch_size`, `prepare_threshold`, `connection_lifetime`, and `manager_client_flags` alias families)
- `tests/scratchbird_surface.mojo:575` (facade closed-connection query/begin/commit/rollback/cancel/stream/metadata guards expose SQLSTATE `08003`, with post-close ping assertion)
- `tests/scratchbird_surface.mojo:854` (facade guard assertion for invalid pool bounds `min_pool_size > max_pool_size`)
- `tests/native_bootstrap.mojo:205` (native-bootstrap helper asserts DSN credential/host/port/database alias parsing + query-order alias precedence across endpoint/credential and manager token aliases + protocol alias canonicalization + mode normalization/precedence checks)
- `tests/native_bootstrap.mojo:99` (native-bootstrap helper asserts session+metadata alias parsing (including `currentSchema`/`defaultRowFetchSize` plus `autocommit|auto_commit` and `readonly|read_only` query-order precedence), prepare-threshold/batched-insert/logger alias parsing, TLS material alias/default parsing with query-order precedence (`ssl_root_cert|sslrootcert`, `ssl_cert|sslcert`, `ssl_key|sslkey`, `ssl_password|sslpassword`), pooling/manager alias parsing, `ssl` alias parsing, accepted `binarytransfer=false` and `compression=zstd` connectivity, compression normalization including repeated-key last-value precedence, URL-style query decoding, and timeout-alias query-order last-match precedence)
- `tests/native_bootstrap.mojo:197` (native-bootstrap smoke asserts bracketed IPv6 host/port parsing)
- `tests/native_bootstrap.mojo:345` (native-bootstrap smoke asserts deterministic `connection_id` endpoint formatting)
- `tests/native_bootstrap.mojo:844` (native-bootstrap deterministic auth-fail guard SQLSTATE assertion via `sb_test_auth_fail=true`)
- `tests/native_bootstrap.mojo:800` (native-bootstrap guard SQLSTATE assertions for transport/protocol/front-door/compression/malformed-query aliases and malformed bracketed-IPv6 + malformed-integer DSN guards, plus negative timeout/session/pooling/manager DSNs, including manager-proxy token requirement, explicit empty-host rejection, and `default_row_fetch_size`/`min_pool_size`/`max_pool_size`/`connection_lifetime`/`manager_client_flags` guards)
- `tests/native_bootstrap.mojo:1010` (native-bootstrap guard assertions that malformed trailing aliases still fail deterministically with SQLSTATE `22023` across timeout, port, `default_row_fetch_size`, `prepare_threshold`, `connection_lifetime`, and `manager_client_flags` alias families)
- `tests/native_bootstrap.mojo:814` (native-bootstrap closed-connection query/begin/commit/rollback/cancel/stream/metadata guards expose SQLSTATE `08003`, with post-close ping assertion)
- `tests/native_bootstrap.mojo:977` (native-bootstrap guard assertion for invalid pool bounds `min_pool_size > max_pool_size`)
- `tests/connection_guards.py:53` (bridge-shim connect guard assertions for `binary_transfer=false`/`binarytransfer=false`, `compression=zstd|none` compatibility, TLS-required `sslmode|ssl=disable` plus unsupported compression/front-door invalid SQLSTATE assertions (`0A000`), front-door token guard `08001`, manager token alias precedence/acceptance (`manager_auth_token|mcp_auth_token`), alias-order precedence checks across `frontdoormode|ingress_mode`, protocol alias normalization/rejection (`0A000`), malformed query-escape/bracketed-IPv6 guards (`22023`), `user/database` and explicit empty-host guards (`28000`) with query-order precedence checks, port validity/range guards (`22023`) including malformed trailing alias rejection (`port|portNumber|pgport`), timeout alias guards (`22023`) including query-order trailing-alias malformed-value rejection, `default_row_fetch_size`/`prepare_threshold`/`connection_lifetime`/`manager_client_flags` trailing-alias malformed-value rejection, and extended integer guards including `cb_failure_threshold`, `cb_recovery_timeout_ms`, `cb_success_threshold`, `cb_half_open_max_requests`, `keepalive_max_idle_before_check_ms`, `leak_threshold_ms`, `pipeline_max_in_flight`, and `pipeline_auto_flush_threshold` (`22023`))
- `tests/integration.py:79` (integration launcher executes `scratchbird_surface.mojo` + `native_bootstrap.mojo` first)
- `tests/integration.py:26` (deterministic fallback manager DSN includes manager token for bridge-shim guard parity)
- `tests/integration.py:425` (deterministic fallback DSN keeps direct integration non-skipping by default)
- `tests/integration.py:434` (deterministic fallback DSN keeps manager-proxy integration non-skipping by default)
- `tests/integration.py:445` (deterministic fallback DSN keeps bad-auth integration non-skipping by default)
- `tests/sbdriver_conformance.py:80` (conformance launcher executes `scratchbird_surface.mojo` + `native_bootstrap.mojo` first)
- `tests/sbdriver_conformance.py:425` (deterministic fallback DSN keeps conformance non-skipping by default)
- `tests/integration.py:62` (matrix DSN parsing + wire-routing helpers for direct/manager/listener/bad-auth lanes via `SCRATCHBIRD_MOJO_*_URLS`)
- `tests/integration.py:526` (integration execution over direct/manager/listener matrices with optional wire routing)
- `tests/sbdriver_conformance.py:50` (conformance matrix DSN parsing + wire-routing helpers)
- `tests/sbdriver_conformance.py:449` (conformance execution over direct/manager/listener DSN matrix set)
- `tests/wire_transport_bridge.py:139` (wire bridge connect/query smoke for runtime selector)
- Gaps/next actions:
- Implemented in this cycle: Mojo lane wire-capable bridge path in `src/scratchbird.py` (`_resolve_transport_mode`, `_PythonWireConnection`, `_WireStatement`, `_WireStream`, `connect` runtime selector) activated via `sb_wire_transport=python` / `SCRATCHBIRD_MOJO_WIRE_TRANSPORT`.
- Implemented in this cycle: managed/listener/bad-auth runtime DSN matrices in `tests/integration.py` and `tests/sbdriver_conformance.py` (`SCRATCHBIRD_MOJO_*_URLS`) with optional live matrix CI gate in `.github/workflows/ci.yml` (`MOJO_LIVE_MATRIX_ENABLED` + live DSN vars).
- Follow-up (non-blocking for this parity batch): complete pure Mojo-native socket/TLS transport in `src/scratchbird.mojo`/`src/scratchbird_native.mojo`.

## TXN (JDBCBL)

- Current status: Implemented (hybrid parity; native bootstrap + wire bridge matrix coverage)
- Lane-local source anchors:
- `src/scratchbird_native.mojo:156` (native-bootstrap nested `begin()` guard `25001`)
- `src/scratchbird_native.mojo:342` (native-bootstrap `commit` enforces closed-connection guard `08003` and remains inactive-txn no-op when open)
- `src/scratchbird_native.mojo:349` (native-bootstrap `rollback` enforces closed-connection guard `08003` and remains inactive-txn no-op when open)
- `src/scratchbird_native.mojo:174` (native-bootstrap savepoint create + generated naming)
- `src/scratchbird_native.mojo:184` (native-bootstrap savepoint release guard `3B001`)
- `src/scratchbird_native.mojo:199` (native-bootstrap rollback-to-savepoint trim behavior)
- `src/scratchbird.py:1031` (bridge-shim shared begin-option integer coercion helper emits SQLSTATE `22023` for invalid transaction knob values)
- `src/scratchbird.py` (`canonical_isolation_label`, explicit isolation alias mapping helper)
- `src/scratchbird.py` (`retry_scope_for_sqlstate`, `is_retryable_sqlstate`)
- `src/scratchbird.py:378` (shared `build_prepared_transaction_sql(...)` emits canonical control SQL and rejects blank global transaction ids with SQLSTATE `42601`)
- `src/scratchbird.py:1056` (bridge-shim static/wire closed-connection guard helper emits SQLSTATE `08003`)
- `src/scratchbird.py:1061` (bridge-shim static/wire savepoint list normalizer ensures deterministic tracking even when `_savepoints` state is missing/non-list)
- `src/scratchbird.py:1322` (bridge-shim wire begin option mapping uses normalized/coerced begin options, persists `_txn_begin_options`, and marks transaction active while resetting savepoints)
- `src/scratchbird.py:1359` (bridge-shim wire `commit` transitions transaction state to inactive while clearing savepoints and `_txn_begin_options`)
- `src/scratchbird.py:1370` (bridge-shim wire `rollback` transitions transaction state to inactive while clearing savepoints and `_txn_begin_options`)
- `src/scratchbird.py` now treats live native session state as authoritative,
  follows ScratchBird's always-in-transaction contract, and preserves
  post-commit / post-rollback reopen truth in `_PythonWireConnection`
- `src/scratchbird.py:1392` (bridge-shim wire savepoint helpers now enforce closed-connection guard precedence via shared static guard helper and deterministic savepoint-list normalization)
- `src/scratchbird.py:2080` (`supports_prepared_transactions()`, `supports_dormant_reattach()`, and `supports_portal_resume()` make prepared/dormant/no-resume capability truth explicit in the public lane surface)
- `src/scratchbird.py:2096` (`prepare_transaction(...)`, `commit_prepared(...)`, `rollback_prepared(...)`, `detach_to_dormant(...)`, and `reattach_dormant(...)` now carry explicit prepared/dormant recovery truth in lane code)
- `src/scratchbird.py:901` (bridge-shim local begin guard enforces closed-connection `08003` + nested transaction `25001`)
- `src/scratchbird.py:1212` (bridge-shim local begin flow validates/coerces begin options before txn activation)
- `src/scratchbird.py:909` (bridge-shim local `commit` enforces closed-connection `08003` and remains inactive-txn no-op when open)
- `src/scratchbird.py:916` (bridge-shim local `rollback` enforces closed-connection `08003` and remains inactive-txn no-op when open)
- `src/scratchbird.py:923` (bridge-shim local savepoint create guard/name generation)
- `src/scratchbird.py:934` (bridge-shim local savepoint release guard `3B001`)
- `src/scratchbird.py:947` (bridge-shim local rollback-to-savepoint behavior)
- Lane-local test anchors:
- `tests/native_bootstrap.mojo:67` (native-bootstrap nested `begin()` rejection with `25001`)
- `tests/txn_exec_parity.py` now covers fresh-boundary adoption / fail-closed
  begin semantics on the shared bridge surface.
- `tests/wire_transport_bridge.py` now proves reconnect and explicit-begin
  behavior against the live Python-wire bridge state model.
- `tests/integration.py` now proves that the next post-rollback query returns
  the real result on the fresh native boundary instead of consuming a stale
  reopen frame.
- `tests/native_bootstrap.mojo:75` (native-bootstrap savepoint lifecycle + `25000`/`3B001` guards)
- `tests/native_bootstrap.mojo:835` (native-bootstrap closed-connection `commit`/`rollback` guards expose SQLSTATE `08003`)
- `tests/scratchbird_surface.mojo:596` (facade closed-connection `commit`/`rollback` guards expose SQLSTATE `08003`)
- `tests/txn_exec_parity.py:73` (begin option mapping assertions plus wire transaction activation/savepoint reset and `_txn_begin_options` persistence)
- `tests/txn_exec_parity.py:107` (wire begin invalid-option SQLSTATE assertions for deterministic `22023` behavior)
- `tests/txn_exec_parity.py:157` (wire commit/rollback state-transition assertions, including `_txn_id` reset, savepoint clearing, and `_txn_begin_options` reset)
- `tests/txn_exec_parity.py:130` (savepoint message/payload parity assertions)
- `tests/txn_exec_parity.py:150` (savepoint guard SQLSTATE assertions)
- `tests/txn_exec_parity.py:198` (wire savepoint tracking initialization/parity assertions when `_savepoints` state is missing)
- `tests/txn_exec_parity.py:269` (shim begin invalid-option SQLSTATE assertions plus prepare-on-closed-connection guard `08003`)
- `tests/txn_exec_parity.py:250` (shim savepoint lifecycle + rollback-trim assertions)
- `tests/txn_exec_parity.py:355` (shim closed-connection `begin`/`commit`/`rollback` guards expose SQLSTATE `08003`)
- `tests/txn_exec_parity.py:488` (wire/static closed-connection guard assertions for begin/commit/rollback/savepoint/query/metadata (including rowcount/restriction helpers, `get_schema`, and `ddl_editor_schema_payload`) expose SQLSTATE `08003`)
- `tests/txn_exec_parity.py:451` (prepared/dormant/no-portal-resume capability helpers, canonical prepared-control SQL, blank-global-id guard `42601`, and dormant fail-closed `0A000` assertions)
- `tests/integration.py:207` (integration smoke begin/savepoint/rollback/commit lifecycle assertions)
- `tests/integration.py:526` (integration runtime executes transaction/savepoint assertions across direct/manager/listener matrices)
- `tests/wire_transport_bridge.py:155` (wire bridge transaction/savepoint lifecycle assertions)
- Gaps/next actions:
- Closed in this cycle: transaction/savepoint integration assertions now execute across direct/manager/listener DSN matrices in `tests/integration.py` with optional live-wire routing (`sb_wire_transport=python`).

## EXEC (JDBCBL)

- Current status: Implemented (hybrid parity; deterministic and wire-matrix execution coverage)
- Lane-local source anchors:
- `src/scratchbird.mojo:13` (facade exports native `ScratchBirdConnection` execution surface)
- `src/scratchbird_native.mojo:127` (native-bootstrap `query` rowcount semantics)
- `src/scratchbird_native.mojo:140` (native-bootstrap `query_with_params` + `07001` mismatch)
- `src/scratchbird_native.mojo:152` (native-bootstrap `prepare` statement surface)
- `src/scratchbird_native.mojo:573` (native-bootstrap closed statement execute guard with SQLSTATE `HY010`)
- `src/scratchbird_native.mojo:579` (native-bootstrap statement `close()` idempotent lifecycle helper)
- `src/scratchbird_native.mojo:213` (native-bootstrap `stream` surface)
- `src/scratchbird_native.mojo:550` (native-bootstrap stream row fetch lifecycle with closed-stream `HY010` and closed-connection `08003` guards)
- `src/scratchbird_native.mojo:235` (native-bootstrap `cancel` surface)
- `src/scratchbird_native.mojo:421` (native-bootstrap `cancel` enforces closed-connection guard `08003`)
- `src/scratchbird_native.mojo:258` (native-bootstrap pipeline queue guard with SQLSTATE `54000`)
- `src/scratchbird_native.mojo:264` (native-bootstrap conditional flush policy honors `pipeline_auto_flush`)
- `src/scratchbird_native.mojo:713` (native-bootstrap invalid integer parameter coercion guard with SQLSTATE `22023`)
- `src/scratchbird.py:748` (bridge-shim statement lifecycle surface)
- `src/scratchbird.py:754` (bridge-shim statement execute enforces `HY010` on closed statement and routes through query execution)
- `src/scratchbird.py:760` (bridge-shim statement `close()` idempotent lifecycle helper)
- `src/scratchbird.py:1208` (bridge-shim `prepare` enforces closed-connection guard `08003`)
- `src/scratchbird.py:777` (bridge-shim query execution with parameter mismatch `07001` and integer coercion guard `22023`)
- `src/scratchbird.py:1451` (bridge-shim wire/static query helper enforces closed-connection guard `08003` prior to simple/extended execution paths)
- `src/scratchbird.py:960` (bridge-shim `stream` surface with closed-connection guard `08003`)
- `src/scratchbird.py:972` (bridge-shim `cancel` surface with closed-connection guard `08003`)
- `src/scratchbird.py:987` (bridge-shim stream fetch lifecycle with closed-stream `HY010` and closed-connection `08003` guards)
- Lane-local test anchors:
- `tests/scratchbird_surface.mojo:46` (facade parameterized/prepare execution assertions)
- `tests/scratchbird_surface.mojo:382` (facade integer-parameter coercion guard assertions return SQLSTATE `22023`)
- `tests/scratchbird_surface.mojo:403` (facade closed statement guard assertion returns SQLSTATE `HY010`)
- `tests/scratchbird_surface.mojo:73` (facade stream/cancel + post-cancel recovery assertions)
- `tests/scratchbird_surface.mojo:565` (facade closed-stream read guard exposes SQLSTATE `HY010`)
- `tests/scratchbird_surface.mojo:605` (facade active-stream-on-closed-connection guard exposes SQLSTATE `08003`)
- `tests/scratchbird_surface.mojo:612` (facade closed-connection `cancel` guard exposes SQLSTATE `08003`)
- `tests/native_bootstrap.mojo:95` (native-bootstrap prepare-bind + mismatch assertions)
- `tests/native_bootstrap.mojo:851` (native-bootstrap closed-connection `cancel` guard exposes SQLSTATE `08003`)
- `tests/native_bootstrap.mojo:427` (native-bootstrap integer-parameter coercion guard assertions return SQLSTATE `22023`)
- `tests/native_bootstrap.mojo:445` (native-bootstrap closed statement guard assertion returns SQLSTATE `HY010`)
- `tests/native_bootstrap.mojo:154` (native-bootstrap stream/cancel `57014` assertions)
- `tests/native_bootstrap.mojo:632` (native-bootstrap closed-stream read guard exposes SQLSTATE `HY010`)
- `tests/native_bootstrap.mojo:839` (native-bootstrap active-stream-on-closed-connection guard exposes SQLSTATE `08003`)
- `tests/native_bootstrap.mojo:175` (native-bootstrap post-cancel recovery assertion)
- `tests/native_bootstrap.mojo:202` (native-bootstrap pipeline-capacity SQLSTATE `54000` assertion)
- `tests/native_bootstrap.mojo:250` (native-bootstrap auto-flush pipeline behavior assertion)
- `tests/native_bootstrap.mojo:263` (native-bootstrap manual-flush retention + close-flush behavior assertion)
- `tests/native_bootstrap.mojo:301` (native-bootstrap breaker-open SQLSTATE `08006` + recovery assertions)
- `tests/scratchbird_surface.mojo:112` (facade pipeline-capacity SQLSTATE `54000` assertion)
- `tests/scratchbird_surface.mojo:160` (facade auto-flush pipeline behavior assertion)
- `tests/scratchbird_surface.mojo:173` (facade manual-flush retention + close-flush behavior assertion)
- `tests/scratchbird_surface.mojo:211` (facade breaker-open SQLSTATE `08006` + recovery assertions)
- `tests/txn_exec_parity.py:196` (shim prepare execute + mismatch + integer coercion/closed-statement/closed-connection statement assertions)
- `tests/txn_exec_parity.py:269` (shim begin invalid-option `22023` assertions and prepare-on-closed-connection `08003` guard assertion)
- `tests/txn_exec_parity.py:283` (shim stream fetch-boundary assertions, including closed-stream SQLSTATE `HY010`)
- `tests/txn_exec_parity.py:299` (shim cancel stream SQLSTATE `57014` assertion)
- `tests/txn_exec_parity.py:355` (shim closed-connection query/cancel/stream guards expose SQLSTATE `08003`)
- `tests/txn_exec_parity.py:488` (wire/static closed-connection query + metadata helper guards, including rowcount/restriction variants plus `get_schema`/`ddl_editor_schema_payload`, expose SQLSTATE `08003`)
- `tests/sbdriver_conformance.py:204` (manifest `requires` gating for prepare/cancel capabilities)
- `tests/integration.py:225` (integration smoke prepare/mismatch and stream/cancel-recovery assertions)
- `tests/integration.py:314` (long-running stream cancellation assertions with runtime matrix-aware execution)
- `tests/wire_transport_bridge.py:155` (wire bridge prepare/stream/cancel lifecycle assertions)
- Gaps/next actions:
- Closed in this cycle: long-running stream fetch-boundary/cancel assertions now run in `tests/integration.py` (`_validate_long_running_stream_cancel`) across managed/listener matrices when live DSNs are present.

## META (JDBCBL)

- Current status: Implemented (hybrid parity; deterministic + runtime matrix payload checks)
- Lane-local source anchors:
- `src/scratchbird.mojo:21` (facade metadata constants exported from native bootstrap)
- `src/scratchbird.mojo:40` (facade metadata query helper family)
- `src/scratchbird.mojo:106` (facade `metadata_query_restricted(...)` routed through restriction-aware resolver)
- `src/scratchbird.mojo:119` (facade `metadata_query_restricted_multi(...)` for multi-restriction shaping)
- `src/scratchbird_native.mojo:281` (native-bootstrap `query_metadata`)
- `src/scratchbird_native.mojo:285` (native-bootstrap `query_metadata_rows`)
- `src/scratchbird_native.mojo:289` (native-bootstrap `query_metadata_restricted`)
- `src/scratchbird_native.mojo:302` (native-bootstrap `query_metadata_rows_restricted`)
- `src/scratchbird_native.mojo:315` (native-bootstrap `query_metadata_restricted_multi`)
- `src/scratchbird_native.mojo:328` (native-bootstrap `query_metadata_rows_restricted_multi`)
- `src/scratchbird_native.mojo:628` (native-bootstrap metadata alias normalization)
- `src/scratchbird_native.mojo:641` (native-bootstrap metadata query resolution)
- `src/scratchbird_native.mojo:1313` (native-bootstrap metadata restriction alias normalization, including catalog/index/constraint/routine/type aliases plus collapsed/camel forms such as `tableSchem`/`tableCatalog`/`dataTypeName`)
- `src/scratchbird_native.mojo:728` (native-bootstrap metadata restriction comparator supports exact/wildcard/null (`=`, `LIKE ... ESCAPE '\'`, `IS NULL`) predicates)
- `src/scratchbird_native.mojo:1496` (native-bootstrap restriction-aware multi-restriction query resolution + `07001` count guard with duplicate-alias last-key precedence)
- `src/scratchbird.py:802` (`_ShimConnection.query_metadata_restricted_multi(...)` instance route)
- `src/scratchbird.py:823` (`_ShimConnection.ddl_editor_schema_payload(...)` editor payload helper)
- `src/scratchbird.py:1144` (bridge-shim `query_metadata_restricted_multi` static route)
- `src/scratchbird.py:1181` (`ScratchBirdConnection.ddl_editor_schema_payload(...)` static editor payload helper)
- `src/scratchbird.py:1156` (bridge-shim instance metadata helpers now enforce closed-connection guard precedence before collection/restriction validation)
- `src/scratchbird.py:1500` (bridge-shim static metadata helpers now enforce closed-connection guard precedence before collection/restriction validation)
- `src/scratchbird.py:1070` (bridge-shim shared metadata rowcount fallback helper returns `rowcount` when non-negative integer (excluding booleans), else `len(rows)`, else `0` with mapping/text/unsized-row fallback)
- `src/scratchbird.py:1083` (bridge-shim shared metadata rows fallback helper normalizes list/tuple rows and returns `[]` for missing/mapping/text/unsupported-iterable rows)
- `src/scratchbird.py:1152` (bridge-shim instance metadata rowcount helpers route through shared fallback semantics)
- `src/scratchbird.py:1212` (bridge-shim instance `get_schema` now routes through shared rows fallback helper)
- `src/scratchbird.py:1215` (bridge-shim instance `ddl_editor_schema_payload` restriction route now uses shared rows fallback helper)
- `src/scratchbird.py:1565` (bridge-shim static `get_schema` now routes through shared rows fallback helper)
- `src/scratchbird.py:1570` (bridge-shim static `ddl_editor_schema_payload` restriction route now uses shared rows fallback helper)
- `src/scratchbird.py:1334` (bridge-shim SQL LIKE matcher helper supports escape-aware, case-insensitive pattern evaluation)
- `src/scratchbird.py:1361` (bridge-shim deterministic schema metadata row shaping supports `=`, `LIKE`, and `IS NULL` filters)
- `src/scratchbird.py:1773` (bridge-shim metadata restriction alias normalization, including catalog/index/constraint/routine/type aliases plus collapsed/camel forms such as `tableSchem`/`tableCatalog`/`dataTypeName`)
- `src/scratchbird.py:1437` (bridge-shim metadata restriction comparator supports exact/wildcard/null (`=`, `LIKE ... ESCAPE '\'`, `IS NULL`) predicates)
- `src/scratchbird.py:1570` (bridge-shim restriction mapping normalizer + `22023` guard for non-mapping inputs)
- `src/scratchbird.py:1986` (bridge-shim restriction-aware multi-restriction query resolution with duplicate-alias last-key precedence)
- `src/scratchbird.py:1738` (`build_ddl_editor_schema_payload(...)` deterministic editor payload builder)
- Lane-local test anchors:
- `tests/scratchbird_surface.mojo:56` (facade metadata alias/query/rowcount assertions)
- `tests/scratchbird_surface.mojo:93` (facade restricted metadata query and rowcount assertions)
- `tests/scratchbird_surface.mojo:128` (facade multi-restriction metadata query/rowcount assertions)
- `tests/scratchbird_surface.mojo:194` (facade metadata alias-family restriction assertions for catalog/index/constraint/routine/type)
- `tests/scratchbird_surface.mojo:544` (facade metadata restriction alias normalization assertions include collapsed/camel forms such as `tableSchem`/`tableCatalog`/`dataTypeName`)
- `tests/scratchbird_surface.mojo:557` (facade metadata multi-restriction duplicate-alias precedence assertions keep last key and allow empty-value clear)
- `tests/scratchbird_surface.mojo:61` (facade metadata restriction count guard `07001`)
- `tests/native_bootstrap.mojo:113` (native-bootstrap metadata alias/query assertions)
- `tests/native_bootstrap.mojo:166` (native-bootstrap restricted metadata query/rowcount assertions)
- `tests/native_bootstrap.mojo:202` (native-bootstrap multi-restriction metadata query/rowcount assertions)
- `tests/native_bootstrap.mojo:273` (native-bootstrap metadata alias-family restriction assertions for catalog/index/constraint/routine/type)
- `tests/native_bootstrap.mojo:610` (native-bootstrap metadata restriction alias normalization assertions include collapsed/camel forms such as `tableSchem`/`tableCatalog`/`dataTypeName`)
- `tests/native_bootstrap.mojo:631` (native-bootstrap metadata multi-restriction duplicate-alias precedence assertions keep last key and allow empty-value clear)
- `tests/native_bootstrap.mojo:84` (native-bootstrap metadata restriction count guard `07001`)
- `tests/metadata_execution.py:40` (bridge-shim metadata alias normalization coverage)
- `tests/metadata_execution.py:55` (bridge-shim extended metadata query resolution coverage)
- `tests/metadata_execution.py:84` (bridge-shim metadata restriction alias normalization coverage, including catalog/index/constraint/routine/type aliases and collapsed/camel forms such as `tableSchem`/`tableCatalog`/`dataTypeName`)
- `tests/metadata_execution.py:103` (bridge-shim restriction-aware metadata query resolution coverage across alias families)
- `tests/metadata_execution.py:219` (bridge-shim multi-restriction query-shaping + mapping guard coverage, including duplicate-alias last-key precedence and empty-value clear semantics)
- `tests/metadata_execution.py:277` (bridge-shim restricted multi-restriction execution routing coverage)
- `tests/metadata_execution.py:328` (bridge-shim restricted multi-restriction rowcount helper coverage)
- `tests/txn_exec_parity.py:561` (bridge-shim static metadata rowcount helper fallback assertions for invalid/missing rowcount and tuple-row fallback semantics)
- `tests/txn_exec_parity.py:461` (bridge-shim instance closed-connection metadata guard precedence assertions for unsupported collection/restriction payloads)
- `tests/txn_exec_parity.py:492` (bridge-shim static closed-connection metadata guard precedence assertions for unsupported collection/restriction payloads)
- `tests/txn_exec_parity.py:666` (bridge-shim instance metadata rowcount helper fallback assertions for invalid/missing/unsupported-iterable row payloads)
- `tests/txn_exec_parity.py:570` (bridge-shim static/instance metadata rowcount helper assertions reject boolean rowcount values and fall back to row length)
- `tests/txn_exec_parity.py:576` (bridge-shim static/instance metadata rowcount helper assertions reject negative rowcount values and fall back to row length)
- `tests/txn_exec_parity.py:638` (bridge-shim static/instance `get_schema` fallback assertions for tuple-row normalization and mapping/text/unsupported-iterable empty fallback)
- `tests/txn_exec_parity.py:772` (bridge-shim static/instance `ddl_editor_schema_payload` fallback assertions for tuple-row normalization and mapping/text/unsupported-iterable empty fallback)
- `tests/metadata_execution.py:339` (bridge-shim editor payload helper path with schema-pattern restriction shaping)
- `tests/metadata_execution.py:370` (bridge-shim `IS NULL` metadata restriction execution coverage)
- `tests/metadata_execution.py:378` (bridge-shim SQL LIKE escape-aware/case-insensitive matcher coverage)
- `tests/metadata_recursive_schema.py:117` (deterministic editor payload shape contract coverage)
- `tests/metadata_recursive_schema.py:147` (shim connection default editor payload contract coverage)
- `tests/integration.py:276` (integration smoke DDL-editor payload contract + tree-parent assertions)
- `tests/integration.py:301` (integration smoke metadata stability invariants across schemas/tables/columns and alias-family restrictions)
- `tests/integration.py:343` (integration smoke deterministic fallback metadata content assertions)
- `tests/integration.py:526` (metadata stability/payload assertions executed across direct/manager/listener runtime matrices)
- `tests/wire_transport_bridge.py:155` (wire bridge metadata restriction + payload assertions)
- Gaps/next actions:
- Closed in this cycle: metadata stability and DDL payload contract assertions now run across direct/manager/listener runtime matrices in `tests/integration.py`, and conformance matrix execution now supports the same DSN families in `tests/sbdriver_conformance.py`.

## TYPE (JDBCBL)

- Current status: Implemented (deterministic codec coverage + wire-bridge parity path)
- Lane-local source anchors:
- `src/scratchbird.py:317` (bridge-shim array/vector/range parser utilities)
- `src/scratchbird.py:537` (bridge-shim type encode wrappers)
- `src/scratchbird.py:599` (bridge-shim type decode wrappers)
- Lane-local test anchors:
- `tests/type_codecs.py:18` (array/range/vector/composite parser and decode assertions)
- `tests/type_codecs.py:53` (geometry/inet/cidr/macaddr decode assertions)
- `tests/type_codecs.py:75` (json/jsonb/uuid decode assertions)
- `tests/type_codecs.py:94` (date/time/timestamp/timestamptz/interval decode assertions)
- `tests/type_codecs.py:125` (array variants including array-of-composite decode assertions)
- `tests/type_codecs.py:138` (OID truncation negative-path assertion)
- `tests/wire_transport_bridge.py:139` (wire bridge query path exercises runtime codec surface via SBWP Python transport adapter)
- Gaps/next actions:
- Closed in this cycle for lane runtime parity: wire-bridge execution path now reuses SBWP Python lane codecs under `sb_wire_transport=python`, with bridge coverage in `tests/wire_transport_bridge.py` and retained deterministic codec assertions in `tests/type_codecs.py`.

## ERR (JDBCBL)

- Current status: Implemented (deterministic SQLSTATE parity + wire negative-path coverage)
- Lane-local source anchors:
- `src/scratchbird.mojo:17` (facade export of native SQLSTATE extractor)
- `src/scratchbird_native.mojo:508` (native-bootstrap unsupported query `0A000`)
- `src/scratchbird_native.mojo:522` (native-bootstrap unsupported parameterized query `0A000`)
- `src/scratchbird_native.mojo:741` (native-bootstrap unsupported metadata `0A000`)
- `src/scratchbird_native.mojo:982` (native-bootstrap guard SQLSTATE error mapping, including endpoint/timeout/auth guardrails)
- `src/scratchbird_native.mojo:1028` (`extract_sqlstate` helper)
- `src/scratchbird_native.mojo:535` (native-bootstrap closed-connection SQLSTATE error emission `08003`)
- `src/scratchbird_native.mojo:576` (native-bootstrap closed statement SQLSTATE error emission `HY010`)
- `src/scratchbird_native.mojo:716` (native-bootstrap invalid integer parameter SQLSTATE error emission `22023`)
- `src/scratchbird_native.mojo:552` (native-bootstrap closed-stream SQLSTATE error emission `HY010`)
- `src/scratchbird_native.mojo:555` (native-bootstrap active-stream-on-closed-connection SQLSTATE error emission `08003`)
- `src/scratchbird.py:236` (bridge-shim `ScratchBirdError` with sqlstate/detail/hint)
- `src/scratchbird.py:775` (bridge-shim closed-connection SQLSTATE error emission `08003`)
- `src/scratchbird.py:1056` (bridge-shim wire/static closed-connection SQLSTATE error emission `08003`)
- `src/scratchbird.py:1208` (bridge-shim prepare-on-closed-connection SQLSTATE error emission `08003`)
- `src/scratchbird.py:756` (bridge-shim closed statement SQLSTATE error emission `HY010`)
- `src/scratchbird.py:796` (bridge-shim invalid integer parameter SQLSTATE error emission `22023`)
- `src/scratchbird.py:1037` (bridge-shim begin-option invalid integer SQLSTATE error emission `22023`)
- `src/scratchbird.py:791` (bridge-shim malformed query-escape SQLSTATE error emission `22023`)
- `src/scratchbird.py:793` (bridge-shim malformed bracketed-IPv6 SQLSTATE error emission `22023`)
- `src/scratchbird.py:800` (bridge-shim invalid protocol SQLSTATE error emission `0A000`)
- `src/scratchbird.py:867` (bridge-shim TLS-required `sslmode|ssl=disable` SQLSTATE error emission `0A000`)
- `src/scratchbird.py:856` (bridge-shim invalid front-door mode SQLSTATE error emission `0A000`)
- `src/scratchbird.py:874` (bridge-shim unsupported compression SQLSTATE error emission `0A000`)
- `src/scratchbird.py:812` (bridge-shim explicit empty-host SQLSTATE error emission `28000`)
- `src/scratchbird.py:833` (bridge-shim missing user/database SQLSTATE error emission `28000`)
- `src/scratchbird.py:835` (bridge-shim port validity/range SQLSTATE error emission `22023`)
- `src/scratchbird.py:1415` (bridge-shim static release-savepoint missing-name SQLSTATE error emission `3B001` with normalized tracking state)
- `src/scratchbird.py:1436` (bridge-shim static rollback-to-savepoint missing-name SQLSTATE error emission `3B001` with normalized tracking state)
- `src/scratchbird.py:927` (bridge-shim timeout guard SQLSTATE error emission `22023`)
- `src/scratchbird.py:898` (bridge-shim statement/lifecycle integer guard SQLSTATE error emission `22023` for `prepare_threshold`, `cb_failure_threshold`, `cb_recovery_timeout_ms`, `cb_success_threshold`, `cb_half_open_max_requests`, `keepalive_max_idle_before_check_ms`, `leak_threshold_ms`, `pipeline_max_in_flight`, and `pipeline_auto_flush_threshold`)
- `src/scratchbird.py:952` (bridge-shim negative/invalid session-pooling-manager integer SQLSTATE error emission `22023`)
- `src/scratchbird.py:989` (bridge-shim closed-stream SQLSTATE error emission `HY010`)
- `src/scratchbird.py:992` (bridge-shim active-stream-on-closed-connection SQLSTATE error emission `08003`)
- Lane-local test anchors:
- `tests/scratchbird_surface.mojo:18` (facade guard SQLSTATE extraction assertions)
- `tests/native_bootstrap.mojo:18` (native-bootstrap connect guard SQLSTATE extraction assertions)
- `tests/native_bootstrap.mojo:36` (native-bootstrap metadata guard SQLSTATE extraction assertions)
- `tests/native_bootstrap.mojo:326` (unsupported query `0A000` extraction assertion)
- `tests/native_bootstrap.mojo:427` (native-bootstrap SQLSTATE extraction assertions for invalid integer parameter guard `22023`)
- `tests/native_bootstrap.mojo:445` (native-bootstrap SQLSTATE extraction assertion for closed statement guard `HY010`)
- `tests/native_bootstrap.mojo:632` (native-bootstrap SQLSTATE extraction assertion for closed-stream read guard `HY010`)
- `tests/native_bootstrap.mojo:839` (native-bootstrap SQLSTATE extraction assertion for active-stream-on-closed-connection guard `08003`)
- `tests/native_bootstrap.mojo:814` (native-bootstrap SQLSTATE extraction assertions for closed-connection query/begin/commit/rollback/cancel/stream/metadata guards `08003`)
- `tests/native_bootstrap.mojo:206` (pipeline-capacity SQLSTATE `54000` extraction assertion)
- `tests/native_bootstrap.mojo:305` (breaker-open SQLSTATE `08006` extraction assertion)
- `tests/scratchbird_surface.mojo:116` (facade pipeline-capacity SQLSTATE `54000` extraction assertion)
- `tests/scratchbird_surface.mojo:215` (facade breaker-open SQLSTATE `08006` extraction assertion)
- `tests/scratchbird_surface.mojo:382` (facade SQLSTATE extraction assertions for invalid integer parameter guard `22023`)
- `tests/scratchbird_surface.mojo:403` (facade SQLSTATE extraction assertion for closed statement guard `HY010`)
- `tests/scratchbird_surface.mojo:565` (facade SQLSTATE extraction assertion for closed-stream read guard `HY010`)
- `tests/scratchbird_surface.mojo:605` (facade SQLSTATE extraction assertion for active-stream-on-closed-connection guard `08003`)
- `tests/scratchbird_surface.mojo:575` (facade SQLSTATE extraction assertions for closed-connection query/begin/commit/rollback/cancel/stream/metadata guards `08003`)
- `tests/txn_exec_parity.py:196` (bridge-shim SQLSTATE assertions for `07001` mismatch, `22023` integer coercion, `HY010` closed statement, and `08003` statement-on-closed-connection guards)
- `tests/txn_exec_parity.py:107` (bridge-shim wire begin-option invalid integer SQLSTATE assertions `22023`)
- `tests/txn_exec_parity.py:269` (bridge-shim begin-option invalid integer SQLSTATE assertions `22023` and prepare-on-closed-connection SQLSTATE assertion `08003`)
- `tests/txn_exec_parity.py:198` (bridge-shim static savepoint missing-state tracking assertions include deterministic `3B001` emission for missing release/rollback-to)
- `tests/txn_exec_parity.py:283` (bridge-shim SQLSTATE assertion for closed-stream read guard `HY010`)
- `tests/txn_exec_parity.py:355` (bridge-shim SQLSTATE assertions for closed-connection query/begin/commit/rollback/cancel/stream/metadata guards `08003`)
- `tests/txn_exec_parity.py:488` (bridge-shim wire/static SQLSTATE assertions for closed-connection begin/commit/rollback/savepoint/query/metadata guards (including rowcount/restriction metadata helper variants plus `get_schema`/`ddl_editor_schema_payload`) `08003`)
- `tests/connection_guards.py:79` (bridge-shim SQLSTATE assertions for invalid protocol/TLS-disable/front-door/compression `0A000`, malformed query-escape/bracketed-IPv6 `22023`, user/database + explicit empty-host `28000`, port/timeouts `22023`, `08001` manager-proxy token guard with alias precedence, and extended integer guard failures including statement/lifecycle knobs (`22023`))
- `tests/errors.py:74` (bridge-shim simple-path SQLSTATE propagation)
- `tests/errors.py:86` (bridge-shim extended-path SQLSTATE propagation)
- `tests/errors.py:92` (bridge-shim auth guard SQLSTATE propagation)
- `tests/wire_transport_bridge.py:198` (wire bridge truncation/decode negative-path SQLSTATE propagation (`08006`))
- Gaps/next actions:
- Closed in this cycle: truncation/decode negative-path coverage now exists for wire bridge mode via `tests/wire_transport_bridge.py` (`test_wire_transport_maps_sqlstate_from_errors`, SQLSTATE `08006` propagation).

## RES (JDBCBL)

- Current status: Implemented (deterministic scaffolds + wire lifecycle matrix assertions)
- Lane-local source anchors:
- `src/scratchbird_native.mojo:238` (native-bootstrap connection close reset semantics)
- `src/scratchbird_native.mojo:422` (native-bootstrap connection close idempotence with closed-state tracking)
- `src/scratchbird_native.mojo:251` (native-bootstrap pre-operation circuit-breaker/keepalive validation hook)
- `src/scratchbird_native.mojo:258` (native-bootstrap pipeline-capacity queue guard with SQLSTATE `54000`)
- `src/scratchbird_native.mojo:264` (native-bootstrap post-operation circuit-breaker + keepalive + telemetry update hook)
- `src/scratchbird_native.mojo:117` (native-bootstrap leak-detector checkout initialization)
- `src/scratchbird_native.mojo:123` (native-bootstrap pipeline start initialization)
- `src/scratchbird_native.mojo:244` (native-bootstrap leak-detector release on close)
- `src/scratchbird_native.mojo:566` (native-bootstrap stream close semantics)
- `src/scratchbird_native.mojo:550` (native-bootstrap stream read lifecycle guard semantics for closed stream/connection)
- `src/scratchbird_native.mojo:579` (native-bootstrap statement close idempotence)
- `src/scratchbird.py:886` (bridge-shim connection close)
- `src/scratchbird.py:1003` (bridge-shim stream close)
- `src/scratchbird/circuit_breaker.mojo:29` (deterministic circuit-breaker state transitions)
- `src/scratchbird/leak_detector.mojo:31` (deterministic leak detector bookkeeping)
- `src/scratchbird/keepalive.mojo:29` (deterministic keepalive tracker)
- `src/scratchbird/keepalive.mojo:66` (deterministic keepalive manager)
- `src/scratchbird/telemetry.mojo:76` (deterministic telemetry scaffolding)
- `src/scratchbird/pipeline.mojo:20` (deterministic pipeline queue/flush scaffolding)
- Lane-local test anchors:
- `tests/txn_exec_parity.py:340` (idempotent close behavior for connection and stream)
- `tests/lifecycle_scaffolds.mojo:20` (lifecycle scaffolds smoke coverage)
- `tests/scratchbird_surface.mojo:85` (facade smoke asserts telemetry/circuit-breaker/keepalive hooks are active)
- `tests/scratchbird_surface.mojo:90` (facade smoke asserts pipeline/leak-detector hooks are active)
- `tests/scratchbird_surface.mojo:567` (facade post-close ping and operation guard lifecycle assertions)
- `tests/scratchbird_surface.mojo:565` (facade stream close lifecycle assertion for closed-stream read guard)
- `tests/native_bootstrap.mojo:179` (native bootstrap smoke asserts telemetry/circuit-breaker/keepalive hooks are active)
- `tests/native_bootstrap.mojo:183` (native bootstrap smoke asserts pipeline/leak-detector hooks are active)
- `tests/native_bootstrap.mojo:806` (native bootstrap post-close ping and operation guard lifecycle assertions)
- `tests/native_bootstrap.mojo:632` (native bootstrap stream close lifecycle assertion for closed-stream read guard)
- `tests/native_bootstrap.mojo:255` (native bootstrap smoke asserts auto-flush pipeline drains pending work)
- `tests/native_bootstrap.mojo:269` (native bootstrap smoke asserts manual pipeline retains pending work before close-flush)
- `tests/native_bootstrap.mojo:311` (native bootstrap smoke asserts circuit-breaker half-open recovery semantics)
- `tests/scratchbird_surface.mojo:165` (facade smoke asserts auto-flush pipeline drains pending work)
- `tests/scratchbird_surface.mojo:179` (facade smoke asserts manual pipeline retains pending work before close-flush)
- `tests/scratchbird_surface.mojo:221` (facade smoke asserts circuit-breaker half-open recovery semantics)
- `tests/integration.py:355` (runtime lifecycle snapshot assertions with monotonic operation tracking)
- `tests/integration.py:314` (long-running stream cancel assertions used as runtime backpressure/lifecycle signal)
- `tests/wire_transport_bridge.py:155` (wire bridge lifecycle snapshot and cancel counters)
- `tests/wire_transport_bridge.py:221` (wire bridge reconnect path proves abandoned transaction/savepoint state is cleared and not replayed into the replacement session)
- Gaps/next actions:
- Closed in this cycle for lane runtime parity: lifecycle/backpressure assertions now include wire-mode snapshots and long-running cancel behavior in `tests/integration.py` (`_validate_lifecycle_snapshot`, `_validate_long_running_stream_cancel`) across live DSN matrices when configured.
