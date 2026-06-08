# DLB-RUST-003 S2 TXN/EXEC Implementation

Date: 2026-03-06  
Lane: `lanes/active/drivers/rust`

## Changes

- Retained existing EXEC parity surfaces in `src/client.rs` and `src/sql.rs`:
  - Callable SQL normalization and callable execution (`native_callable_sql`, `call`).
  - Multi-result summaries (`query_multi`, `execute_multi`) and batch summaries (`execute_batch`, `query_batch`).
  - Generated-key extraction (`execute_with_generated_keys`).
- Closed remaining TXN parity gap by adding first-class autocommit control:
  - `Client::autocommit()` getter.
  - `Client::set_autocommit(enabled)` setter with JDBC-aligned transition semantics:
    - no-op when unchanged,
    - `true` commits active transaction before mode switch,
    - emits `SET_OPTION autocommit=on/off`,
    - `false` eagerly begins a transaction when none is active.
- Added deterministic runtime TXN/EXEC assertions in `tests/runtime_contract_gate_test.rs`:
  - Autocommit transition wire-event order (`set_option`/`txn_begin`/`txn_commit`).
  - Multi-result execution path on a local scripted server.

## Tests Run

1. `cargo test`
   - Result: `PASS`

## TXN Status

Recommendation: `IMPLEMENTED`.

## EXEC Status

Recommendation: `IMPLEMENTED`.

