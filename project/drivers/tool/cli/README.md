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

`SBsql` and `SBsec` share one explicit local first-principal bootstrap command:

```text
SBsql bootstrap <first-principal> <database-path> --mode=embedded \
  --platform-profile <file> --resource-seed-pack-root <dir> \
  --policy-seed-pack-root <dir> [--password-stdin]
```

It is an embedded engine lifecycle operation rather than a normal connection.
It creates the new database and its transaction-1 security catalog atomically,
so no existing database login is needed. It still requires Root/Administrator
authority and protected initial-password input. Ordinary `SBsql` embedded
connections, listeners, managers, and parser processes never create a missing
database.

For a local POSIX create-and-connect sequence, use:

```text
SBsql create <first-principal> <database-path> --mode=embedded \
  --resource-seed-pack-root <dir> \
  --policy-seed-pack-root <dir> --start-server <SBsrv-executable> \
  --server-config <file> --ipc-path <socket-path> -- -c "SELECT 1"
```

Creation runs under the current OS account and its filesystem permissions.
It requires no database login, administrator privilege, or service account.
To select the installer's administrator-authorized service-account handoff,
add `--platform-profile <file>`. That selection is explicit; a failed service
handoff does not fall back to the current account.

The embedded constructor verifies the committed catalog while retaining its
exclusive file ownership, then closes the database. Only a verified and closed
creation can launch the dedicated server. The server opens under its own locks
and reports its actual database UUID through a private inherited socket after
runtime readiness. SBsql then connects using normal local IPC authentication.
The creation password is erased before launch; connection authentication reads
a separate password. For automation, `--password-stdin` and
`--connect-password-stdin` consume separate input lines. Credentials are never
passed to the child in command-line arguments.

`--startup-timeout-ms` defaults to 30000 and accepts 1 through 600000. A launch,
readiness, or authentication failure preserves the committed database. After
readiness the server remains running independently of the client. Its PID is
printed for management. Until readiness, the launcher owns the child and stops
and reaps it on failure. A killed startup is not a clean database shutdown.

This command uses the existing local-password creation profile and POSIX local
IPC bridge. It does not provision external authentication providers or database
encryption keys. Windows refuses this combined command before creating a file;
the separate embedded bootstrap command remains available. A database that
committed with verification or close warnings must be verified before launch.
Creation success alone does not establish runtime activation or client rights.

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
