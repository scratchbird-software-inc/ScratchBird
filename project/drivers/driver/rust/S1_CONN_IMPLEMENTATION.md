# DLB-RUST-002 S1 CONN Implementation

Date: 2026-04-17  
Lane: `lanes/active/drivers/rust`

## What Changed

1. Closed shared connection/bootstrap policy parity in `src/config.rs`, `src/client.rs`, `src/protocol.rs`, and `src/scram.rs`:
   - Protocol aliases (`jdbc`, `odbc`, `postgresql`, etc.) normalize to `native`.
   - `sslmode` is normalized/validated (`disable|require|verify-ca|verify-full` + aliases).
   - `compression` is normalized/validated (`off|zstd` + aliases); unknown values are rejected.
   - `binary_transfer=false` and `compression=zstd` are accepted and negotiated instead of hard-rejected.
2. Implemented the shared staged auth/bootstrap contract:
   - Added public staged probe surfaces: `probe_auth_surface(dsn)` and `client.probe_auth_surface().await`.
   - Added resolved-auth reporting via `client.get_resolved_auth_context()`.
   - Added direct runtime execution for `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`, and `TOKEN`.
   - Added canonical token/bootstrap keys including `auth_token`, `auth_method_payload`, `auth_payload_json`, `auth_payload_b64`, `workload_identity_token`, and `proxy_principal_assertion`.
   - Added deterministic `manager_proxy` MCP bootstrap/connect support using `manager_auth_token`.
   - Added fail-closed handling for negotiated-but-not-local `MD5`, `PEER`, and `REATTACH`.
3. Expanded deterministic runtime CONN proof in `tests/runtime_contract_gate_test.rs`:
   - Full manager-proxy MCP handshake success path.
   - Deterministic manager-proxy auth failure path.
   - Deterministic direct `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`, and `TOKEN` flows.
   - Direct and manager staged auth-surface probe coverage.
   - Fail-closed `PEER` handling with preserved resolved-auth context.
   - Capability parity assertions for startup feature bits and query flags under `binary_transfer=false` + `compression=zstd`.

## Tests Run

1. `cargo test`
   - Result: `PASS`

## CONN Status Recommendation

Recommendation: `IMPLEMENTED` (baseline-complete for 0.1.0 scope).
