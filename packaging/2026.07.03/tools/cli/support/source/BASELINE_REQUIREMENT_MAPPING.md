# ScratchBird Driver Baseline Requirement Mapping (S0)

## Scope
- Lane-local S0 artifact for `lanes/active/tooling/cli` only.
- Maps this lane's current capabilities to JDBCBL groups: `CONN`, `TXN`, `EXEC`, `META`, `TYPE`, `ERR`, `RES`.
- All statements below are anchored to files in this lane.

## MGA Recovery Contract
- This lane follows ScratchBird's MGA/state-based engine recovery model.
- Reconnect or reopen only repairs transport and session state.
- Reconnect never resurrects abandoned in-flight transactions or replay lost statements.
- Transaction recovery in the lane means reset, rollback, reopen, or retry against engine truth.
- Result resume is valid only for explicit suspended protocol states.
- This lane uses explicit disconnect/reset and fresh client allocation in
  lifecycle/conformance loops rather than transparent same-instance reconnect.
- `SET TRANSACTION` remains SQL-driven in this lane rather than a typed begin
  API; source comments now make the fail-closed retry rule explicit:
  `40xxx` requires a fresh statement boundary, `08xxx` requires reconnect or
  reopen, and the CLI never auto-replays a whole transaction.
- the shared C++ network client under this lane now adopts a compatible
  default fresh native MGA boundary instead of sending a redundant
  `TXN_BEGIN` when the engine has already reopened the session transaction.
- See `../../../../public_audit_summary`.

## CONN (JDBCBL: CONN)
- Current status: Implemented
- Lane-local source anchors:
  - `README.md:7-19` documents supported connection modes and connection-string options.
  - `sb_isql.cpp:3071-3189` normalizes connection mode and builds connection target parameters.
  - `sb_isql.cpp:3191-3365` parses `--connection`, `--mode`, `--ipc-*`, and `--conn-opt`.
  - `sb_admin.cpp:145-251` and `sb_security.cpp:202-316` implement equivalent mode normalization and target construction.
  - `sb_isql.cpp:3541-3552`, `sb_admin.cpp:253-266`, `sb_security.cpp:708-725` perform connect calls.
  - `cli_auth_bootstrap.cpp` now centralizes shared connection-target assembly plus staged auth/bootstrap text/json rendering for the network-backed tools.
  - `sb_isql.cpp`, `sb_admin.cpp`, and `sb_security.cpp` now expose `--probe-auth-surface`, `--show-auth-context`, and generic `--auth-token` pass-through on top of the shared C++ probe/getter contract.
- Lane-local test anchors:
  - `sbdriver_conformance.cpp:657-677` builds client config from DSN and optional appended params.
  - `sbdriver_conformance.cpp:804-827` executes per-test connect flow.
  - `cli_auth_bootstrap_test.cpp` covers auth-token target assembly, explicit connection-string passthrough, local invalid-namespace probe failure, and resolved-auth rendering.
  - `CMakeLists.txt:25,186-199` builds and links `sbdriver_conformance` in this lane.
- Gaps/next actions:
  - Add explicit conformance manifests for each documented transport mode (`embedded`, `local-ipc`, `inet`, `managed`).

## TXN (JDBCBL: TXN)
- Current status: Partial
- Lane-local source anchors:
  - `sb_isql.cpp:1196-1359` handles `COMMIT`, `ROLLBACK`, savepoints, and `SET TRANSACTION`.
  - `sb_isql.cpp:935-949` exposes `SET AUTODDL`; `sb_isql.cpp:175` stores `autoddl` in config.
  - `sb_isql.cpp:2678-2688` applies stop/exit behavior after execution errors.
  - `sbdriver_conformance.cpp:712-748` adapts network client begin/commit/rollback operations for conformance runs.
  - `txn_exec_parity.cpp:133-205` exposes explicit prepared/dormant/no-portal capability helpers and canonical prepared-transaction SQL construction.
  - `txn_exec_parity.cpp:168-349` implements `txn_exec` flow (`begin -> optional savepoint operations -> sql -> commit/rollback -> verify_sql`) with error-safe rollback and savepoint option validation.
- Lane-local test anchors:
  - `sbdriver_conformance.cpp:829-833,895-897` normalizes `txn` alias and dispatches `txn_exec`.
  - `txn_exec_parity_test.cpp:173-338` covers commit/rollback verification, rollback-on-error behavior, savepoint release/rollback-to flows, and savepoint option guardrails.
  - `txn_exec_parity_test.cpp:327-380` proves explicit prepared/dormant/no-portal capability truth and fail-closed dormant handling.
  - `conformance/sbwp_conformance_manifest.sample.json` now proves live `txn_exec` commit/rollback verification on the shared native runtime through `sbdriver_conformance`.
  - `CMakeLists.txt:208-215` adds the dedicated `sbdriver_txn_exec_tests` lane test target.
- Gaps/next actions:
  - `AUTODDL` is configurable (`sb_isql.cpp:935-949`) but not consumed by a separate transaction-control path in this lane.

## EXEC (JDBCBL: EXEC)
- Current status: Implemented
- Lane-local source anchors:
  - `sb_isql.cpp:1543-1589` executes SQL via client query call and renders results.
  - `sb_admin.cpp:288-305` and `sb_security.cpp:322-339` execute query and non-query SQL paths.
  - `sb_isql.cpp:2409-2425` supports `\plan` by issuing an `EXPLAIN` query.
  - `txn_exec_parity.cpp:79-126` adds `native_exec` validation over row-count and rows-affected expectations.
  - `sbdriver_conformance.cpp:829-833,892-894` normalizes `exec` alias and dispatches `native_exec`.
- Lane-local test anchors:
  - `txn_exec_parity_test.cpp:132-170` validates `native_exec` success and mismatch handling.
  - `txn_exec_parity_test.cpp:173-225` validates execution parity inside transaction commit/rollback flows.
  - `sbdriver_conformance.cpp:874-1230` continues to cover query, prepare, paging, progress, notify, copy, lob, and cancel paths.
  - `conformance/sbwp_conformance_manifest.sample.json` now proves live `native_exec`, `txn_exec`, and `res_loop_exec` behavior on the shared native runtime.
- Gaps/next actions:
  - Expand live-connection manifest coverage beyond the checked-in sample to the rest of the CLI front-door command families.
  - `\sblr` remains a placeholder pending client support (`sb_isql.cpp:2435-2443`).

## META (JDBCBL: META)
- Current status: Partial
- Lane-local source anchors:
  - `sb_isql.cpp:1727-1816` maps `\d`-family commands to metadata SQL.
  - `metadata_shaping.h:9-44` defines metadata schema tree row contracts (database/schema row kinds, parent/path semantics).
  - `metadata_shaping.cpp:106-251` implements metadata-only schema shaping: dotted-parent expansion, per-parent de-dup, and object-resolver schema extraction.
  - `sb_isql.cpp:3443-3535` now routes `--schema-tree` through metadata shaping rows (database/default branch + recursive dotted tree output).
  - `sb_isql.cpp:2737-2959` implements DDL extraction for domains, sequences, tables, views, indexes, triggers, procedures, and functions.
- Lane-local test anchors:
  - `metadata_shaping_test.cpp:34-134` covers database/default branch rows, dotted parent expansion, per-parent uniqueness, and same-leaf/different-parent behavior.
  - `CMakeLists.txt:221-224` adds dedicated `sbdriver_metadata_shaping_tests` build target.
- Gaps/next actions:
  - DDL extraction includes placeholders/fallbacks for missing definitions (`sb_isql.cpp:2750-2752`, `2814-2816`, `2837`, `2925`, `2948`).
  - Add conformance/live metadata coverage for `--schema-tree` and `\d` metadata query families (current validation is lane-local unit only).

## TYPE (JDBCBL: TYPE)
- Current status: Partial
- Lane-local source anchors:
  - `sbdriver_conformance.cpp:138-580` decodes array/vector/range/network/macaddr/uuid and other OID-tagged values.
  - `sbdriver_conformance.cpp:585-620` encodes JSON params to text/binary bind payloads.
  - `sbdriver_conformance.cpp:689-709,861-880` projects typed row values into normalized JSON output with per-column OID metadata.
  - `conformance_assertions.cpp:96-244` applies manifest-driven typed assertions (`expect_column_type_oids`, `expect_first_row_json`, `expect_first_row_types`, `expect_rows_json`).
- Lane-local test anchors:
  - `sbdriver_conformance.cpp:849-885` exercises parameter binding and SQLSTATE checks for prepared execution.
  - `sbdriver_conformance.cpp:1062-1097` validates LOB payload and checksum behavior.
  - `conformance_assertions_test.cpp:1-104` validates expectation pass/fail behavior, numeric coercion, and explicit type-tag mismatch detection.
  - `CMakeLists.txt:241-250` adds the dedicated `sbdriver_conformance_assertion_tests` target.
- Gaps/next actions:
  - Unsupported/unknown OIDs fall back to raw byte-string conversion (`sbdriver_conformance.cpp:580`).
  - Expand typed manifests beyond lane smoke coverage (`conformance/sbwp_conformance_manifest.sample.json`) to assert every decoded type family in DSN-backed runtime gates.

## ERR (JDBCBL: ERR)
- Current status: Implemented
- Lane-local source anchors:
  - `sb_isql.cpp:1574-1583` surfaces execution errors from `core::ErrorContext`.
  - `sb_isql.cpp:2678-2688` enforces `CONTINUE/STOP/EXIT` error handling actions.
  - `sb_admin.cpp:122-124,288-303` and `sb_security.cpp:175-177,322-337` print operation errors.
  - `sbdriver_conformance.cpp:679-687` standardizes error result payloads.
- Lane-local test anchors:
  - `sbdriver_conformance.cpp:850-881` checks expected SQLSTATE behavior for prepared execution failures.
  - `sbdriver_conformance.cpp:1170-1174` checks SQLSTATE for cancel-flow outcomes.
- Gaps/next actions:
  - CLI tools currently print message-first errors; add SQLSTATE/code to user-facing output for parity with conformance assertions.

## RES (JDBCBL: RES)
- Current status: Partial
- Lane-local source anchors:
  - `sb_admin.cpp:82,254-273` connection ownership converted to `std::unique_ptr<Connection>` with explicit disconnect/reset lifecycle.
  - `sb_security.cpp:158,709-731` connection ownership converted to `std::unique_ptr<Connection>` with explicit disconnect/reset lifecycle.
  - `sb_isql.cpp:197-199,1500-1538,1890-1907,3554-3643` output/error stream ownership converted to `std::unique_ptr<std::ofstream>` with deterministic stderr restoration.
  - `sbdriver_conformance.cpp:1018,1176,1257,1296,1327,1368,1564,1606` closes prepared statements and disconnects clients across normal/fallback/cancel paths.
  - `sbdriver_conformance.cpp:1078-1168` adds `res_loop_exec` manifest kind normalization/dispatch and routes to lifecycle loop parity without one-time preconnect coupling.
  - `res_lifecycle_parity.cpp:67-160` provides deterministic connect/execute/disconnect loop orchestration with explicit cleanup on execute failures.
- Lane-local test anchors:
  - `sbdriver_conformance.cpp:887-958` exercises repeated statement prepare/execute/close cycles.
  - `sbdriver_conformance.cpp:1106-1169` runs threaded cancel flow with explicit statement close/join.
  - `res_lifecycle_parity_test.cpp:92-194` stress-tests repeated connect/execute/disconnect iterations plus cleanup semantics on connect/execute failure paths.
- Gaps/next actions:
  - `sb_isql` still uses a global non-owning `Connection*` alias to a stack connection object; migrate this global state to an explicit RAII session/context object.
  - Promote the checked-in live `res_loop_exec` sample into routine CI/runtime artifact collection once the lane's broader shell gate is stabilized.
