# DLB-R-002 S1 CONN Implementation (R Lane)

Scope: `lanes/active/drivers/r` only.

## Changes Applied

1. DBI connection capability baseline parity
- Added `dbCanConnect` for `ScratchbirdDriver` in `R/dbi.R`.
- Behavior: attempts `sb_connect`, returns `FALSE` on error, and performs best-effort `sb_disconnect` on success before returning `TRUE`.

2. Protocol/auth helper correctness hardening
- Updated `read_u32` in `R/protocol.R` to decode unsigned 32-bit values without `readBin` signedness warnings.
- Kept behavior minimal and lane-local; no protocol surface expansion.

3. Lane tests for connection/auth/protocol
- Added `tests/testthat/test_conn_protocol.R` with focused offline tests:
  - `sb_connect` pre-socket validation (`user/database`, `binary_transfer`, `compression`).
  - `sb_connect` bootstrap path via mocks (`sb_open_socket`, startup/auth, schema apply, socket close).
  - `DBI::dbCanConnect` success/failure behavior and cleanup semantics via mocks.
  - Auth parser frame decode/truncation checks (`parse_auth_request`, `parse_auth_continue`, `parse_auth_ok`).
  - TLS-required enforcement (`sb_open_socket` with `sslmode=disable`).

## Targeted Tests Run

1. `Rscript -e 'testthat::test_local(filter = "(conn_protocol|config|transport_tls)", reporter = "summary")'`
- Result: `PASS`
- Notes: Non-fatal startup warning from `/etc/os-release` in this environment; tests completed successfully.

## Final CONN Status Recommendation

- Recommendation: `PARTIAL`
- Rationale:
  - `dbCanConnect` gap is now closed and covered by lane tests.
  - Offline connection/auth/protocol checks are stronger and deterministic.
  - Full live connection/auth parity is still environment-gated (`SCRATCHBIRD_R_URL`) and manager-proxy handshake is not yet covered end-to-end in lane tests.

## Remaining Gaps

- Add non-skip live connection/auth test coverage where feasible (or stable harness fixtures) to reduce environment-gated evidence.
- Add end-to-end `manager_proxy` handshake validation beyond parser/config and mocked-path assertions.
