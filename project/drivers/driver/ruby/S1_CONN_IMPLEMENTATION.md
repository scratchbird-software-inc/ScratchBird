# DLB-RUBY-002 S1 CONN Implementation

## What Changed

- Expanded DSN/property normalization in
  [`lib/scratchbird/config.rb`](lib/scratchbird/config.rb):
  - protocol aliases normalize to `native`
  - `sslmode` accepts the canonical transport modes
  - manager/bootstrap inputs are parsed
  - shared auth/bootstrap inputs now include `auth_token`,
    startup auth selection hints, and manager-proxy inputs
- Added staged bootstrap/auth surfaces in
  [`lib/scratchbird/client.rb`](lib/scratchbird/client.rb),
  [`lib/scratchbird/connection.rb`](lib/scratchbird/connection.rb),
  and [`lib/scratchbird.rb`](lib/scratchbird.rb):
  - public `Scratchbird.probe_auth_surface(dsn_or_options)`
  - `Scratchbird::Client#probe_auth_surface`
  - `Scratchbird::Client#get_resolved_auth_context`
  - `Scratchbird::Connection#resolved_auth_context`
  - direct-listener probe and manager-proxy probe
  - stable admitted-method reporting with plugin ids and broker-required flags
- Updated connect/auth behavior in
  [`lib/scratchbird/client.rb`](lib/scratchbird/client.rb):
  - connect and staged probe now share the same socket/bootstrap path
  - `front_door_mode=manager_proxy` still fails fast with `08001` when a
    real attach requires `manager_auth_token`
  - startup auth selection is applied through startup params
  - native auth execution now includes `PASSWORD`, `SCRAM_SHA_256`,
    `SCRAM_SHA_512`, and `TOKEN`
  - admitted but unsupported or broker-required methods (`MD5`, `PEER`,
    `REATTACH`) now fail closed with `0A000`
- Generalized SCRAM handling in
  [`lib/scratchbird/scram.rb`](lib/scratchbird/scram.rb)
  so deterministic handshake coverage now includes both SHA-256 and SHA-512
- Fixed the unrelated lane type/decode regressions in
  [`lib/scratchbird/types.rb`](lib/scratchbird/types.rb)
  so the full lane suite is green instead of “auth complete but suite red”
- Added lane auth/bootstrap proof in
  [`test/test_conn_auth_protocol.rb`](test/test_conn_auth_protocol.rb):
  - direct probe
  - manager-proxy probe
  - `SCRAM_SHA_512` handshake
  - `TOKEN` execution
  - fail-closed `PEER`

## Targeted Tests Run

- `cd lanes/active/drivers/ruby && ruby -Ilib:test test/test_conn_auth_protocol.rb`
  - Result: `PASS`
- `cd lanes/active/drivers/ruby && ruby -Ilib:test test/test_config.rb`
  - Result: `PASS`
- `cd lanes/active/drivers/ruby && ruby -Ilib:test test/test_types.rb`
  - Result: `PASS`
- `cd lanes/active/drivers/ruby && for f in test/test_*.rb; do ruby -Ilib:test "$f" || exit $?; done`
  - Result: `PASS` (integration cases remain env-gated/skipped)

## CONN Status Recommendation

- `IMPLEMENTED` (baseline-complete for the 0.1.0 scope).
