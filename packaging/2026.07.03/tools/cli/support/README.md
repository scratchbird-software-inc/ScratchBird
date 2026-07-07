# ScratchBird CLI Tools

Native CLI tools for ScratchBird operations and conformance workflows:
`sb_isql`, `sb_admin`, `sb_backup`, `sb_security`, `sb_verify`, and
`sbdriver_conformance`.

## Top-Level Lane Docs

- [`BASELINE_REQUIREMENT_MAPPING.md`](BASELINE_REQUIREMENT_MAPPING.md) - Lane-local S0 mapping of CLI capabilities to JDBCBL requirement groups.
- CLI user docs
- CLI API reference
- Shared auth/bootstrap contract
- Documentation index

## MGA Recovery Contract

This lane follows ScratchBird's MGA/state-based engine recovery model.

- reconnect or reopen only repairs transport and session state
- reconnect never resurrects abandoned in-flight transactions or replay lost statements
- transaction recovery in the lane means reset, rollback, reopen, or retry against engine truth
- result resume is valid only for explicit suspended protocol states
- this lane uses explicit disconnect/reset and fresh client allocation in
  lifecycle/conformance loops rather than transparent same-instance reconnect
- `SET TRANSACTION` remains SQL-driven in this lane rather than a typed begin
  API; source comments now make the fail-closed retry rule explicit:
  `40xxx` requires a fresh statement boundary, `08xxx` requires reconnect or
  reopen, and the CLI never auto-replays a whole transaction
- the shared C++ network client under this lane now adopts an already-active
  fresh native MGA boundary for default begin calls instead of sending a
  redundant `TXN_BEGIN` back into the engine
- prepared / limbo truth is explicit in lane source through
  `txn_exec_parity::{supportsPreparedTransactions,buildPreparedTransactionSql}`
  rather than implied by reconnect folklore
- dormant reattach truth is explicit and fail-closed through
  `txn_exec_parity::{supportsDormantReattach,rejectDormantReattach}`
- standalone portal resume is intentionally absent and source-visible through
  `txn_exec_parity::supportsPortalResume() -> false`

See `../../../../public_audit_summary`.

## Connection Modes

Network-backed CLIs (`sb_isql`, `sb_admin`, `sb_security`) now support the
current ScratchBird connection protocol surface:

- `--mode=embedded` (mapped to local IPC transport in the current beta C++
  network client implementation)
- `--mode=local-ipc` (`ipc_method` + required `ipc_path` to the running
  server SBPS Unix endpoint)
- `--mode=inet` (listener TCP mode)
- `--mode=managed` (manager proxy front-door mode)

Use `--connection=<connection_string>` for full explicit control, or combine
mode flags with `--conn-opt key=value` for additional driver parameters.

## Auth / Bootstrap

The network-backed tools `sb_isql`, `sb_admin`, and `sb_security` now expose
the shared staged auth/bootstrap contract directly:

- `--probe-auth-surface` performs pre-connect auth negotiation/probing and exits
- `--show-auth-context` prints the resolved auth context after a real connect
- `--auth-token=<tok>` exposes the generic token-auth payload surface
- `--auth-method-id`, `--auth-method-payload`, `--auth-payload-json`,
  `--auth-payload-b64`, `--auth-provider-profile`,
  `--auth-required-methods`, `--auth-forbidden-methods`,
  `--auth-require-channel-binding`, `--workload-identity-token`, and
  `--proxy-principal-assertion` are all passed through to the shared C++ driver
- unsupported admitted methods remain fail-closed through the shared driver
  surface instead of being guessed locally

## Platform Support

| Platform | Status | Notes |
|----------|--------|-------|
| Linux | Supported | CI build coverage. |
| Windows | Experimental | CI build attempt enabled; verify run status before release. |
| macOS | Untested | Not currently covered in CI. |

## Build

```bash
cmake -S . -B build_cli -DSB_BUILD_CLI=ON -DSB_BUILD_CPP=ON -DSB_BUILD_ODBC=OFF
cmake --build build_cli --config Release
ctest --test-dir build_cli --output-on-failure
```

Optional:
- `-DSB_BUILD_CLI_FDW=ON` builds `sb_pg_isql`, `sb_my_isql`, `sb_fb_isql` (requires FDW adapters from the engine repo).

See `docs/BUILD_MATRIX.md` for dependencies.

## Conformance Sample

Lane-local sample manifest and one-command runner:

- Manifest: `conformance/sbwp_conformance_manifest.sample.json`
- Runner: `conformance/run_sbdriver_conformance_sample.sh`
- The adapter now supports manifest-level typed assertions via
  `expect_columns`, `expect_column_type_oids`, `expect_first_row_json`,
  `expect_first_row_types`, and `expect_rows_json`.
- The checked-in sample manifest now also proves explicit `txn_exec`
  commit/rollback verification plus `res_loop_exec` behavior on the shared
  native runtime stack.

Run:

```bash
export SB_CONFORMANCE_DSN="scratchbird://user:pass@localhost:3092/mydb?protocol=native"
lanes/active/tooling/cli/conformance/run_sbdriver_conformance_sample.sh
```

Optional runner flags:

- `--binary-params` or `--text-params`
- `--manifest <path>`
- `--output <path>`
- `--no-build`
