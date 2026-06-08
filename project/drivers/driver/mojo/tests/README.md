# ScratchBird Mojo Tests

This lane executes through Mojo entrypoints (`*.mojo`) that delegate to paired
Python test scripts (`*.py`) via Mojo-Python interop.

## Requirements

- Python 3.10+
- `pixi` with Mojo toolchain (default manifest path: `~/mojo-work/sb-mojo`)

Optional environment overrides:
- `MOJO_PIXI_MANIFEST`: path to Mojo pixi workspace used by launcher scripts
- `MOJO_BIN`: explicit Mojo binary (used when pixi manifest is unavailable)
- `SCRATCHBIRD_MOJO_NATIVE_RUN_ARGS`: optional override for native smoke compiler flags (default `-O0 -j1`)

## Quick Run

Run from lane root (`lanes/active/drivers/mojo`):

```bash
pixi run -m ~/mojo-work/sb-mojo --executable mojo run -O0 -j1 -I src -I src/scratchbird tests/scratchbird_surface.mojo
pixi run -m ~/mojo-work/sb-mojo --executable mojo run -O0 -j1 -I src -I src/scratchbird tests/native_bootstrap.mojo
pixi run -m ~/mojo-work/sb-mojo --executable mojo run -O0 -j1 tests/auth_bootstrap_contract.mojo
pixi run -m ~/mojo-work/sb-mojo --executable mojo run -O0 -j1 tests/metadata_recursive_schema.mojo
pixi run -m ~/mojo-work/sb-mojo --executable mojo run -O0 -j1 tests/metadata_execution.mojo
pixi run -m ~/mojo-work/sb-mojo --executable mojo run -O0 -j1 tests/txn_exec_parity.mojo
pixi run -m ~/mojo-work/sb-mojo --executable mojo run -O0 -j1 tests/errors.mojo
pixi run -m ~/mojo-work/sb-mojo --executable mojo run -O0 -j1 tests/type_codecs.mojo
pixi run -m ~/mojo-work/sb-mojo --executable mojo run -O0 -j1 tests/connection_guards.mojo
pixi run -m ~/mojo-work/sb-mojo --executable mojo run -O0 -j1 -I src/scratchbird tests/lifecycle_scaffolds.mojo
pixi run -m ~/mojo-work/sb-mojo --executable mojo run -O0 -j1 tests/integration.mojo
```

Expected behavior:
- native bootstrap, metadata, txn/exec, error-propagation, and type-codec tests report `OK` (including native ping, begin/commit/rollback, savepoint lifecycle guards, prepared execute, paging-query rowcount, and post-cancel recovery smoke semantics)
- auth/bootstrap contract smoke reports `OK` and validates public `probe_auth_surface(...)`, deterministic direct/password and manager/TOKEN ingress reporting, fail-closed `PEER` truth in deterministic mode, and `sb_wire_transport=python` delegation to the upgraded Python lane for staged probe plus `get_resolved_auth_context()`
- native bootstrap/facade smoke now also validates DSN credential parsing (`user:password` with query overrides and aliases `username`/`passwd`/`pguser`/`pgpassword`), endpoint parsing (`host`/`port` with aliases `hostname`/`servername`/`pghost`, `portNumber`/`pgport`, and bracketed IPv6 hosts), database alias parsing (`database`/`dbname`/`databaseName`/`pgdatabase`), session property parsing (`role`, `application_name|applicationname`, `autocommit|auto_commit`, `readonly|read_only`, `current_schema|search_path|searchPath|currentschema`, `default_row_fetch_size|fetch_size|fetchSize|defaultrowfetchsize`, `metadata_expand_schema_parents|metadataexpandschemaparents|expand_schema_parents|expandschemaparents|dbeaver_expand_schema_parents|dbeaverexpandschemaparents`), statement/logging property parsing (`prepare_threshold|preparethreshold`, `rewrite_batched_inserts|rewritebatchedinserts`, `logger_level|loggerlevel|log_level|loglevel`, `logger_file|loggerfile|log_file|logfile`) with defaults (`prepare_threshold=5`, `rewrite_batched_inserts=false`, `logger_level=OFF`, `logger_file=''`), TLS material parsing (`sslrootcert`, `sslcert`, `sslkey`, `sslpassword`, plus underscore aliases) with default empty values, pooling/manager parsing (`tcpkeepalive`, `pooling`, `min_pool_size|minpoolsize`, `max_pool_size|maxpoolsize`, `connection_lifetime|connectionlifetime|poolingconnectionlifetime`, `manager_*|mcp_*` with default assertions for `manager_connection_profile`, `manager_client_intent`, `manager_auth_fast_path`), timeout alias parsing (`connect_timeout|connecttimeout`, `socket_timeout|sockettimeout`, `login_timeout|logintimeout`, `acquire_timeout|acquiretimeout` with fallback `pooling_acquire_timeout|poolingacquiretimeout`), protocol alias parsing (`protocol|parser|dialect` with canonicalization of `scratchbird`, `scratchbird-native`, `scratchbird_native` to `native`), front-door mode normalization (`manager-proxy`/`managerproxy`/`managed`) with query-order alias precedence (`front_door_mode|frontdoormode|connection_mode|ingress_mode`, last matching key wins) and invalid-value `0A000` guard parity, transport alias parsing (`binary_transfer|binarytransfer`, including accepted `binarytransfer=false` connectivity), `ssl` alias parsing (`ssl` → `sslmode`, including accepted `disable`), compression normalization (`compression=none` → `off`) with accepted JDBC-compatible `compression=zstd` and unknown compression guard behavior (`compression=gzip` rejected), URL-style query decoding (`%xx`, `+`) for DSN values, timeout/endpoint/protocol/session/pooling/manager guard SQLSTATE behavior (`22023` / `28000` / `08001` / `0A000`, including malformed query-escape rejection, malformed bracketed-IPv6 rejection, malformed integer DSN value rejection, omitted-host defaulting to `localhost`, explicit empty-host rejection, out-of-range port rejection, manager-proxy token requirement, negative `default_row_fetch_size`, `min_pool_size`, `connection_lifetime`, and `manager_client_flags` rejections), and deterministic auth-fail guard parity (`sb_test_auth_fail=true` → `28P01`)
- native bootstrap/facade smoke validates deterministic `connection_id` formatting (`user@host:port/database`) to anchor leak-detector/pipeline lifecycle identity behavior
- native bootstrap smoke also validates deterministic SQLSTATE-prefixed guard/unsupported errors via `scratchbird_native.extract_sqlstate(...)`
- native bootstrap/facade smokes validate active circuit-breaker/keepalive/telemetry plus leak-detector/pipeline hook wiring on query/stream paths, including deterministic SQLSTATE guards for pipeline-capacity (`54000`) and circuit-breaker-open (`08006`), plus auto/manual pipeline flush behavior and breaker recovery semantics
- metadata execution smoke includes deterministic metadata restriction alias/query shaping helpers (including expanded alias coverage for catalog/index/constraint/routine/type keys, cross-collection schema/table/index/constraint/routine/type predicates, wildcard `LIKE ... ESCAPE '\\'` shaping with escaped-pattern semantics, explicit `IS NULL` predicate shaping, multi-restriction combination semantics, and invalid-restriction SQLSTATE guards) plus `query_metadata_rows(...)`/`query_metadata_rows_restricted(...)`/`query_metadata_rows_restricted_multi(...)` rowcount checks in shim/native-bootstrap/facade paths
- metadata recursive-schema smoke includes deterministic DDL-editor payload contract checks (`schemaPattern` / `expandSchemaParents` / `schemaPaths` / `schemaTree`) for both pure builder and shim-connection helper paths
- lifecycle scaffold test reports `OK` and validates circuit-breaker/leak-detector/keepalive/telemetry/pipeline deterministic behavior under current Mojo syntax
- type-codec suite covers vector/range/composite/geometry/network plus temporal/json/jsonb/uuid and array-of-composite wrappers in the bridge shim
- integration smoke includes transaction/savepoint lifecycle checks, prepare/stream-cancel recovery checks, metadata wrapper stability checks across schemas/tables/columns rowcount relationships, alias-family restriction execution checks, `ddl_editor_schema_payload(...)` contract/tree-parent assertions, deterministic fallback schema-content assertions, and prints skip messages when direct/manager/bad-auth env DSNs are not set

## Conformance Adapter

The `sbdriver-conformance` launcher resolves Mojo automatically (prefers pixi
manifest mode) and runs `tests/sbdriver_conformance.mojo`.

By default, both `sbdriver-conformance` and `integration.py` run current-syntax
native smoke (`tests/scratchbird_surface.mojo`, then
`tests/native_bootstrap.mojo`) first, then continue through the bridge-shim
harness path.

Conformance `prepare_bind` checks prefer `connection.prepare(...).execute(...)`
when available and fall back to `connection.query(sql, params)` for older lanes.
Conformance manifest `requires` entries are enforced in-harness; unsupported
requirements are reported as `skipped`.
Conformance defaults to a deterministic lane DSN when `SCRATCHBIRD_MOJO_URL`
is unset, so core tests run as `ok` in local lane runs without external env.
Integration smoke also defaults to deterministic lane DSNs for direct,
manager-proxy, and bad-auth paths when corresponding env vars are unset.

CI Mojo gate mirrors the explicit lane sequence:
`scratchbird_surface.mojo` → `native_bootstrap.mojo` → `metadata_execution.mojo`
→ `metadata_recursive_schema.mojo` → `integration.py` → `sbdriver-conformance`.

```bash
tests/sbdriver-conformance --manifest ../../../../docs/fixtures/sbwp_conformance_manifest.json
```

Environment variables:
- `SCRATCHBIRD_CONFORMANCE_MANIFEST`: optional manifest path
- `SCRATCHBIRD_MOJO_URL`: DSN for running query tests
- `SCRATCHBIRD_MOJO_MANAGER_URL`: optional manager-proxy integration DSN
- `SCRATCHBIRD_MOJO_BAD_AUTH_URL`: optional bad-auth integration DSN (for shim tests, append `sb_test_auth_fail=true`)
- `SCRATCHBIRD_MOJO_SKIP_NATIVE_BOOTSTRAP`: optional override to skip native smoke in `integration.mojo` / `integration.py` / `sbdriver_conformance.py`
- `SCRATCHBIRD_MOJO_NATIVE_REQUIRED`: optional override to fail launcher smoke when native bootstrap cannot run
- `SCRATCHBIRD_MOJO_ENABLE_PREPARE_BIND`: optional override (defaults enabled; set `0` to disable)
- `SCRATCHBIRD_MOJO_ENABLE_CANCEL`: optional override (defaults enabled; set `0` to disable)
- `SCRATCHBIRD_MOJO_DISABLE_FALLBACK_DSN`: optional override to require explicit `SCRATCHBIRD_MOJO_URL` / `SCRATCHBIRD_MOJO_MANAGER_URL` / `SCRATCHBIRD_MOJO_BAD_AUTH_URL` (restores skip behavior when corresponding env vars are unset)

Deterministic native lifecycle DSN knobs for smoke coverage:
- `cb_failure_threshold`
- `cb_recovery_timeout_ms`
- `cb_success_threshold`
- `cb_half_open_max_requests`
- `keepalive_max_idle_before_check_ms`
- `leak_threshold_ms`
- `pipeline_max_in_flight`
- `pipeline_auto_flush`
- `pipeline_auto_flush_threshold`

With `SCRATCHBIRD_MOJO_DISABLE_FALLBACK_DSN=1` and no
`SCRATCHBIRD_MOJO_URL`, conformance tests are reported as skipped; integration
direct/manager/bad-auth branches also skip when their env DSNs are unset.
