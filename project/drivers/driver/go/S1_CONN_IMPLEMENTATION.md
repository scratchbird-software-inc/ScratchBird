# DLB-GO-002 S1 CONN Implementation

## What Changed

- Expanded DSN/property normalization in [`config.go`](config.go):
  - Protocol aliases (`jdbc`, `odbc`, `postgresql`, `scratchbird-native` variants) now normalize to `native`.
  - `sslmode` accepts JDBC-compatible aliases and normalizes to `disable|require|verify-ca|verify-full`.
  - `compression` accepts `off|zstd` plus aliases and rejects unknown values.
  - `binary_transfer` now uses strict bool parsing while allowing `false`.
  - Auth/bootstrap fields are parsed, including `auth_token`, startup plugin selection inputs, and manager-proxy inputs.
- Added staged bootstrap/auth surfaces in [`auth_bootstrap.go`](auth_bootstrap.go) and [`scratchbird.go`](scratchbird.go):
  - public `ProbeAuthSurface(ctx, dsn)` and `(*Connector).ProbeAuthSurface(ctx)`
  - `(*Conn).GetResolvedAuthContext()`
  - direct-listener probe and manager-proxy probe
  - stable admitted-method reporting with plugin ids and broker-required flags
- Updated connect/auth behavior in [`conn.go`](conn.go):
  - `sslmode=disable` now bypasses TLS instead of hard-rejecting.
  - `compression=zstd` and `binary_transfer=false` are accepted.
  - `connect(...)` and staged probe now share the same socket/bootstrap normalization path.
  - `front_door_mode=manager_proxy` still fails fast with `08001` when `manager_auth_token` is required for a real attach.
  - Startup auth plugin selection is applied through protocol params.
  - native auth execution now includes `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`, and `TOKEN`.
  - admitted but unsupported or broker-required methods (`MD5`, `PEER`, `REATTACH`) now fail closed with `0A000` instead of guessing through generic payload replay.
  - Fixed `ensureOpen` lock ordering to avoid connect/handshake deadlock.
- Added always-on runtime contract coverage in [`runtime_contract_gate_test.go`](runtime_contract_gate_test.go):
  - Manager-proxy handshake/auth path with no environment dependency.
  - Startup feature-bit and query-flag assertions for compression/binary-transfer parity.
- Added lane auth/bootstrap proof in [`conn_protocol_test.go`](conn_protocol_test.go):
  - direct probe
  - manager-proxy probe
  - `SCRAM_SHA_512` handshake
  - `TOKEN` execution
  - fail-closed `PEER`

## Targeted Tests Run

- `cd lanes/active/drivers/go && go test ./...`
  - Result: `PASS`

## CONN Status Recommendation

- `IMPLEMENTED` (baseline-complete for the 0.1.0 scope).
