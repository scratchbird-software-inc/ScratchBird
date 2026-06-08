# DLB-CLI-003 S2 TXN/EXEC Implementation

Date: 2026-03-04
Lane: `lanes/active/tooling/cli`

## Changes

1. Added lane-local TXN/EXEC parity helper module:
   - `txn_exec_parity.h`
   - `txn_exec_parity.cpp`
   - New capabilities:
     - `native_exec` validation with `expect_rows_affected` and optional `expect_rows` checks.
     - `txn_exec` flow with explicit `beginTransaction`, `commit|rollback`, optional `verify_sql`, and best-effort rollback on in-transaction failure.
2. Integrated TXN/EXEC helper flows into `sbdriver_conformance`:
   - Added kind aliases: `exec -> native_exec`, `txn -> txn_exec`.
   - Added `NetworkTxnExecClient` adapter to map `NetworkClient` operations into shared parity helpers.
3. Added targeted lane tests for transaction and execution behavior:
   - New test binary: `sbdriver_txn_exec_tests`.
   - New tests in `txn_exec_parity_test.cpp` cover:
     - `native_exec` success and mismatch behavior.
     - `txn_exec` commit/verify and rollback/verify flows.
     - rollback-on-error behavior when transactional execution fails.
     - savepoint lifecycle parity (`SAVEPOINT`, `RELEASE SAVEPOINT`, `ROLLBACK TO SAVEPOINT`) and guardrails for savepoint-only options.
4. Updated lane build wiring in `CMakeLists.txt`:
   - Fixed `SB_CPP_DRIVER_DIR` path to the actual lane-relative beta C++ driver location.
   - Added `txn_exec_parity.cpp` to `sbdriver_conformance`.
   - Added `sbdriver_txn_exec_tests` target.
5. Updated TXN/EXEC evidence and gaps in `BASELINE_REQUIREMENT_MAPPING.md`.

## Tests Run

1. `cmake -S . -B build_txn_exec -DSB_BUILD_CLI_FDW=OFF && cmake --build build_txn_exec --target sbdriver_conformance sbdriver_txn_exec_tests -j4`
   - Result: PASS
2. `./build_txn_exec/sbdriver_txn_exec_tests`
   - Result: PASS (`txn_exec_parity_test: PASS`)

## TXN Status

Recommendation: `PARTIAL`

Rationale:
- `txn_exec` now provides lane-local begin/commit/rollback conformance behavior and targeted tests.
- Savepoint/release/rollback-to conformance coverage is implemented and validated in deterministic lane tests.

## EXEC Status

Recommendation: `MET`

Rationale:
- Existing execution paths were already implemented in lane CLIs.
- `native_exec` adds direct rows-affected/row-count execution parity checks with dedicated targeted tests.

## Remaining Gaps

1. Add live-connection manifest tests that execute `native_exec` and `txn_exec` against runtime fixtures in CI.
2. `sb_isql` `\\sblr` remains a documented placeholder pending client support.
