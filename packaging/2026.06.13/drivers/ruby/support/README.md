# ScratchBird Ruby Driver

Native ScratchBird Ruby driver using the ScratchBird wire protocol.

## Documentation

- Getting started
- API reference
- [Baseline requirement mapping (S0)](BASELINE_REQUIREMENT_MAPPING.md)

## Auth / Bootstrap Contract

This lane now implements the shared staged auth/bootstrap contract.

- public probe surface: `Scratchbird.probe_auth_surface(dsn_or_options)`,
  `Scratchbird::Client#probe_auth_surface`
- resolved auth reporting:
  `Scratchbird::Client#get_resolved_auth_context`,
  `Scratchbird::Connection#resolved_auth_context`
- direct listener and `manager_proxy` bootstrap
- executable local auth classes:
  - `PASSWORD`
  - `SCRAM_SHA_256`
  - `SCRAM_SHA_512`
  - `TOKEN`
- fail-closed negotiated reporting for admitted but unsupported or
  broker-required methods such as `MD5`, `PEER`, and `REATTACH`

## MGA Recovery Contract

This lane follows ScratchBird's MGA/state-based engine recovery model.

- reconnect or reopen only repairs transport and session state
- reconnect never resurrects abandoned in-flight transactions or replay lost statements
- transaction recovery in the lane means reset, rollback, reopen, or retry against engine truth
- result resume is valid only for explicit suspended protocol states
- `Scratchbird::Client#resume_portal` now fails closed with `55000` unless the
  server first reported `MSG_PORTAL_SUSPENDED`
- same-client reconnect discards prepared handles, attachment parameters, and
  cached plan/SBLR frames from the abandoned session before the replacement
  handshake
- `Scratchbird::Connection#prepare_transaction`, `#commit_prepared`, and
  `#rollback_prepared` expose explicit prepared/limbo control through
  canonical transaction-control SQL
- `Scratchbird::Connection#supports_dormant_reattach?` is explicit and false,
  and `#detach_to_dormant` / `#reattach_dormant` fail closed until a public
  dormant front-door exists
- `Scratchbird::Client#begin_transaction(options)` now exposes the canonical
  `READ COMMITTED` sub-mode selector directly through
  `:read_committed_mode`, including `READ COMMITTED READ CONSISTENCY`
- `Scratchbird::Protocol.canonical_read_committed_mode_label(...)` makes the
  selected canonical MGA mode visible in lane source and tests
- native `READY` status plus `current_txn_id` are authoritative for
  transaction activity in this lane; ScratchBird sessions stay always in a
  transaction and autocommit-off execution therefore relies on the
  server-owned session boundary instead of injecting a synthetic
  client-side `BEGIN`
- `Scratchbird::ErrorMapper.retry_scope_for_sqlstate(...)` makes the retry
  boundary explicit: `40001`/`40P01` => fresh statement only, `08xxx` =>
  reconnect or reopen only, everything else => no automatic replay

See `../../../../public_audit_summary`.

## Build/Test (Windows/Linux)

See `docs/BUILD_MATRIX.md`.

## Platform Support

| Platform | Status | Notes |
|----------|--------|-------|
| Linux | Supported | CI build/test coverage. |
| Windows | Supported | CI build/test coverage. |
| macOS | Untested | Not currently covered in CI. |

## Installation

```bash
gem build scratchbird.gemspec
gem install scratchbird-0.1.0.gem
```

## Usage

```ruby
require "scratchbird"

conn = Scratchbird.connect("scratchbird://user:pass@localhost:3092/mydb")
result = conn.query("SELECT 1 AS one")
puts result.first[0]
conn.close
```

## Connection strings

URI:

```
scratchbird://user:password@host:3092/database?sslmode=require
```

Key-value:

```
host=localhost port=3092 dbname=mydb user=myuser password=mypass
```
