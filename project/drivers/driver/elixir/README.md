# ScratchBird Elixir (Ecto) Driver

Native ScratchBird driver with an Ecto adapter. Uses SBWP v1.1 and now exposes
the shared staged auth/bootstrap contract required across driver lanes.

## Documentation

- Getting started
- API reference
- [Baseline requirement mapping](BASELINE_REQUIREMENT_MAPPING.md)

## Auth / Bootstrap Surface

Public staged auth/bootstrap entry points:

- `ScratchBird.probe_auth_surface/1`
- `ScratchBird.Connection.probe_auth_surface/1`
- `ScratchBird.get_resolved_auth_context/1`
- `ScratchBird.Connection.get_resolved_auth_context/1`

Implemented native auth/bootstrap classes:

- `PASSWORD`
- `SCRAM_SHA_256`
- `SCRAM_SHA_512`
- `TOKEN`
- `manager_proxy` token bootstrap

Negotiated but fail-closed when locally unsupported:

- `MD5`
- `PEER`
- `REATTACH`

## MGA Recovery Contract

This lane follows ScratchBird's MGA/state-based engine recovery model.

- reconnect or reopen only repairs transport and session state
- reconnect never resurrects abandoned in-flight transactions or replay lost statements
- transaction recovery in the lane means reset, rollback, reopen, or retry against engine truth
- result resume is valid only for explicit suspended protocol states
- native `READY`, `TXN_STATUS`, and `current_txn_id` are authoritative for
  transaction activity; ScratchBird sessions stay always in a transaction and
  `COMMIT` / `ROLLBACK` reopen the next boundary
- `begin/2` is documented against that always-in-transaction contract rather
  than idle-session semantics
- the focused live recovery slice in `test/integration_test.exs` now proves
  that rollback leaves the next query immediately usable on the reopened
  native boundary with no reconnect and no statement replay
- `begin/2` accepts explicit transaction options and currently documents the
  alias mapping `:read_committed` => canonical `READ COMMITTED`,
  `:repeatable_read` => canonical `SNAPSHOT`,
  `:serializable` => canonical `SNAPSHOT TABLE STABILITY`
- `begin/2` now also exposes the canonical `READ COMMITTED` sub-mode selector
  directly through `:read_committed_mode`, including
  `READ COMMITTED READ CONSISTENCY`
- `ScratchBird.canonical_read_committed_mode_label/1` keeps the selected
  canonical MGA mode source-visible for auditors and lane tests
- `ScratchBird.retry_scope/1` makes the retry boundary explicit:
  `40001`/`40P01` => fresh statement only, `08xxx` => reconnect or reopen
  only, everything else => no automatic replay
- prepared / limbo truth is explicit in lane code through
  `ScratchBird.Connection.supports_prepared_transactions/0`,
  `build_prepared_transaction_sql/2`, `prepare_transaction/2`,
  `commit_prepared/2`, and `rollback_prepared/2`, which emit canonical
  transaction-control SQL
- dormant detach / reattach truth is explicit in lane code through
  `supports_dormant_reattach/0`, `detach_to_dormant/1`, and
  `reattach_dormant/3`, all of which fail closed with `0A000`
- this lane does not expose a standalone public portal-resume helper;
  `supports_portal_resume/0 -> false` keeps that boundary explicit instead of
  implying reconnect- or stream-based continuation folklore

Current lane boundary:
- the native Elixir connection does not implement transparent in-place reconnect on an existing state struct
- replacement sessions come from a fresh `ScratchBird.Connection.connect/1` handshake or DBConnection reconnect cycle
- disconnect tears down the current wire/session instead of preserving a local transaction claim across the boundary

See `../../../../public_audit_summary`.

## Platform Support

| Platform | Status | Notes |
|----------|--------|-------|
| Linux | Supported | CI build/test coverage. |
| Windows | Supported | CI build/test coverage. |
| macOS | Untested | Not currently covered in CI. |

## Install (local dev)

```bash
cd elixir
mix local.hex --force
mix local.rebar --force
mix deps.get
```

Requires Elixir ~> 1.15 (per `mix.exs`).

## Quick Start

```elixir
config = [
  url: "scratchbird://user:pass@localhost:3092/mydb",
  application_name: "my_app"
]

{:ok, conn} = ScratchBird.Connection.connect(config)
{:ok, result, conn} = ScratchBird.Connection.query(conn, "SELECT 1", [])
IO.inspect(result.rows)
```

Managed mode (`front_door_mode=manager_proxy`) is supported using the
`manager_*` connection parameters, and staged probe/bootstrap can inspect that
front door before final credential commitment.

`sslmode` accepts:
`disable`, `allow`, `prefer`, `require`, `verify-ca`, `verify-full`.

`disable` remains a local/dev parity option; production policy should still
prefer TLS-enabled modes.

## Ecto Adapter

```elixir
# config/config.exs
config :my_app, MyApp.Repo,
  adapter: ScratchBird.Ecto,
  url: "scratchbird://user:pass@localhost:3092/mydb"
```

## Metadata Helpers

The lane now exposes direct metadata query helpers for the richer catalog
families as well as the basic sys.* surfaces:

- `schemas_query`, `tables_query`, `columns_query`
- `indexes_query`, `index_columns_query`, `constraints_query`
- `procedures_query`, `functions_query`, `routines_query`
- `catalogs_query`
- `primary_keys_query`, `foreign_keys_query`
- `table_privileges_query`, `column_privileges_query`
- `type_info_query`

The remaining metadata gap in this lane is live metadata proof and broader
runtime integration, not absence of the local query families.

## Tests

Integration tests use:

- `SCRATCHBIRD_TEST_DSN`
- `SCRATCHBIRD_TEST_MANAGER_DSN`

When those env vars are absent, the live integration slices are skipped rather
than failed; the loopback auth/bootstrap contract tests still run locally.

See `public_contract_snapshot` for requirements.
