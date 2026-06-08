# S1 CONN Implementation (DLB-PHP-002)

Date: 2026-04-17  
Scope: `lanes/active/drivers/php` only.

## What Changed

- Closed shared connection/bootstrap policy parity in `src/Config.php`, `src/Connection.php`, `src/Protocol.php`, and `src/Scram.php`:
  - DSN boolean parsing remains normalized for `binary_transfer` and `manager_auth_fast_path`.
  - `binary_transfer=false` and `compression=zstd` remain accepted at connect validation.
  - Added canonical `auth_token` alias parsing alongside the existing auth payload keys.
  - Updated the native auth ids to the shared contract (`PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`, `TOKEN`, `MD5`, `PEER`, `REATTACH`).
  - Generalized SCRAM to support SHA-256 and SHA-512.
- Implemented the shared staged auth/bootstrap contract:
  - Added public staged probe surface: `Connection::probeAuthSurface(dsn)`.
  - Added resolved-auth reporting via `getResolvedAuthContext()`.
  - Added direct runtime execution for `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`, and `TOKEN`.
  - Added deterministic `manager_proxy` MCP bootstrap/connect support using `manager_auth_token`.
  - Added fail-closed handling for negotiated-but-not-local `MD5`, `PEER`, and `REATTACH`.
- Fixed pre-existing lane-red type decoding issues in `src/TypeDecoder.php` by narrowing binary-text fallback so fixed-width scalar and geometry payloads are decoded as their native binary forms rather than misclassified as printable text.
- Expanded S1-focused lane tests:
  - `tests/ConfigTest.php` now covers `auth_token` parsing.
  - `tests/ProtocolConnAuthTest.php` covers `SCRAM_SHA_512` auth-continue parsing.
  - `tests/ConnectionConnTest.php` covers staged direct/manager probes, resolved-auth reporting, token auth, `SCRAM_SHA_512`, and fail-closed `PEER`.
  - `tests/ScramTest.php` adds direct SCRAM-SHA-512 proof coverage.

## Test Commands Run

1. `composer install`
   - Result: PASS
2. `composer test`
   - Result: PASS (`111 tests`, `437 assertions`, `20 skipped`, `0 failures`).

## CONN Status Recommendation

- Recommendation: `IMPLEMENTED`

## Remaining Gaps

- None for current JDBC baseline scope.
- Remaining work is live server proof and release evidence staging, not lane-local connection/auth surface closure.
