# ScratchBird Elixir Driver — Language Binding (Ecto Adapter)

> **Status: beta\_2 / release\_candidate** — The Elixir driver is production-capable
> on Linux and Windows (CI-verified). macOS is untested. The Ecto adapter and
> native DBConnection client are stable for SBWP v1.1. Use cautiously in
> production until the full release gate is cleared.

## Purpose

The Elixir driver provides a native ScratchBird client for Elixir applications
using both a bare `ScratchBird.Connection` module and a full Ecto adapter
(`ScratchBird.Ecto`) for ORM-style access. It speaks SBWP v1.1 directly over
TCP/TLS via the DBConnection framework and does not wrap any JDBC or ODBC layer.

Target audience: Elixir/Phoenix developers who want native ScratchBird
connectivity with or without Ecto, and teams that need explicit access to
ScratchBird's MGA transaction semantics.

## Manifest Metadata

| Field                    | Value                                      |
|--------------------------|--------------------------------------------|
| `driver_package_uuid`    | `019e12a0-0004-7000-8000-000000000004`     |
| `driver_family`          | `elixir`                                   |
| `api_surface_set`        | `language_binding`                         |
| `ingress_mode_set`       | `direct_listener`, `manager_proxy`         |
| `wire_protocol_set`      | `sbwp_v1_1`                                |
| `dsn_key_set`            | `database`, `host`, `port`, `user`, `auth_method` |
| `auth_method_set`        | `engine_local_password`, `scram_ready`     |
| `tls_profile_set`        | `scratchbird_tls_1_3_floor`                |
| `type_mapping_profile`   | `sbsql_core`                               |
| `diagnostic_mapping_profile` | `native_sqlstate`                      |
| `metadata_profile`       | `sys_information_recursive`                |
| `thread_safety_class`    | `thread_safe`                              |
| `pooling_capability`     | `session_pool`                             |
| `release_bucket`         | `release_candidate`                        |
| `conformance_profile_ref`| `driver_elixir_gate`                       |

## Install

The Mix package is `:scratchbird_ecto` (`mix.exs` app `:scratchbird_ecto`).
Add to your `mix.exs` (local dev path shown; Hex publication pending):

```elixir
# mix.exs
defp deps do
  [
    {:scratchbird_ecto, path: "/path/to/scratchbird"},
    {:ecto_sql, "~> 3.11"},
    {:db_connection, "~> 2.6"}
  ]
end
```

Elixir `~> 1.14` is required (the `mix.exs` declares `~> 1.14`; note that the
README states `~> 1.15` as the practical minimum for current OTP
compatibility). Then:

```bash
mix local.hex --force
mix local.rebar --force
mix deps.get
```

## Connecting

The primary entry-point modules are `ScratchBird.Connection` and
`ScratchBird` (the public facade).

**DSN form (URL):**

```
scratchbird://user:password@host:port/database[?option=value&...]
```

Default port: 3092. Accepted `sslmode` values: `disable`, `allow`, `prefer`,
`require`, `verify-ca`, `verify-full`. `disable` is valid for local/dev use;
production deployments should prefer TLS-enabled modes. See
../connection\_and\_dsn.md for the full key reference.

**Minimal connection example (bare `ScratchBird.Connection`):**

```elixir
config = [
  url: "scratchbird://user:pass@localhost:3092/mydb",
  application_name: "my_app"
]

{:ok, conn} = ScratchBird.Connection.connect(config)
{:ok, result, conn} = ScratchBird.Connection.query(conn, "SELECT 1", [])
IO.inspect(result.rows)
```

**Ecto adapter** (source: `lib/scratchbird_ecto/connection.ex`):

```elixir
# config/config.exs
config :my_app, MyApp.Repo,
  adapter: ScratchBird.Ecto,
  url: "scratchbird://user:pass@localhost:3092/mydb"
```

### Manager-proxy ingress

Set `front_door_mode: "manager_proxy"` in the connection config and supply
`manager_auth_token`. Manager-proxy mode uses staged probe/bootstrap: call
`ScratchBird.probe_auth_surface/1` or
`ScratchBird.Connection.probe_auth_surface/1` to inspect the front-door auth
requirement before committing credentials. See ../authentication.md.

### Auth discovery

```elixir
{:ok, surface} = ScratchBird.probe_auth_surface(config)
context = ScratchBird.get_resolved_auth_context(conn)
```

Supported native auth methods: `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`,
`TOKEN`, `manager_proxy` token bootstrap. Methods `MD5`, `PEER`, and
`REATTACH` are admitted by wire negotiation but fail closed.

## Executing Statements and Transactions

ScratchBird sessions are always in a transaction. Native `READY`, `TXN_STATUS`,
and `current_txn_id` are authoritative for transaction activity. `COMMIT` and
`ROLLBACK` reopen the next boundary; `begin/2` is documented against this
always-in-transaction contract rather than idle-session semantics.

```elixir
# Simple query
{:ok, result, conn} = ScratchBird.Connection.query(conn, "SELECT id FROM users", [])

# Explicit transaction with options
{:ok, conn} = ScratchBird.Connection.begin(conn, isolation: :repeatable_read)
case ScratchBird.Connection.query(conn, "UPDATE ...", []) do
  {:ok, _, conn} -> ScratchBird.Connection.commit(conn)
  {:error, _}   -> ScratchBird.Connection.rollback(conn)
end
```

**Isolation-level aliases for `begin/2`** (source: `lib/scratchbird/connection.ex`):

| Elixir atom        | Wire canonical label             |
|--------------------|----------------------------------|
| `:read_committed`  | `READ COMMITTED`                 |
| `:repeatable_read` | `SNAPSHOT`                       |
| `:serializable`    | `SNAPSHOT TABLE STABILITY`       |

The `READ COMMITTED` sub-mode is selected via `:read_committed_mode` in the
`begin/2` option map. `ScratchBird.canonical_read_committed_mode_label/1`
returns the canonical label for a given sub-mode integer.

**Retry boundary** (source: `lib/scratchbird/errors.ex` — `retry_scope/1`):

| SQLSTATE         | Retry boundary                  |
|------------------|---------------------------------|
| `40001`, `40P01` | Fresh statement only            |
| `08xxx`          | Reconnect / reopen only         |
| All others       | No automatic replay             |

### Reconnect behaviour

This lane uses fresh-connect-only recovery. `disconnect/2` tears down the
current wire session; replacement sessions come from a new
`ScratchBird.Connection.connect/1` handshake or DBConnection reconnect cycle.
Reconnect never replays abandoned transactions.

### Prepared transactions

`supports_prepared_transactions/0` is `true`. Use
`prepare_transaction/2`, `commit_prepared/2`, and `rollback_prepared/2` for
two-phase commit with canonical transaction-control SQL.
`supports_dormant_reattach/0` is `false`; related helpers fail closed with
SQLSTATE `0A000`.

## Type Mapping

Full type mapping: [../type\_mapping.md](../type_mapping.md).

| SBsql core type    | Elixir type                | OID constant (src)      |
|--------------------|----------------------------|-------------------------|
| `BOOLEAN`          | `true` / `false`           | `@oid_bool` (16)        |
| `SMALLINT`         | `integer`                  | `@oid_int2` (21)        |
| `INTEGER`          | `integer`                  | `@oid_int4` (23)        |
| `BIGINT`           | `integer`                  | `@oid_int8` (20)        |
| `REAL`             | `float`                    | `@oid_float4` (700)     |
| `DOUBLE PRECISION` | `float`                    | `@oid_float8` (701)     |
| `NUMERIC`          | `Decimal.t()` (`decimal`)  | `@oid_numeric` (1700)   |
| `TEXT` / `VARCHAR` | `String`                   | `@oid_text`/`@oid_varchar` |
| `BYTEA`            | binary                     | (bytea OID)             |
| `DATE`             | `Date.t()`                 | `@oid_date` (1082)      |
| `TIME`             | `Time.t()`                 | `@oid_time` (1083)      |
| `TIMESTAMP`        | `NaiveDateTime.t()`        | `@oid_timestamp` (1114) |
| `TIMESTAMPTZ`      | `DateTime.t()`             | `@oid_timestamptz` (1184)|
| `UUID`             | `String`                   | `@oid_uuid` (2950)      |
| `JSON` / `JSONB`   | decoded term (via `:jason`) | `@oid_json`/`@oid_jsonb`|
| `VECTOR`           | `[float]` list             | `@oid_sb_vector` (16386)|
| Arrays             | `[T]` list                 | per element OID         |
| `COMPOSITE`        | list of values             | `@oid_record` (2249)    |
| Range types        | map with bounds            | `@oid_int4range` etc.   |
| `INET` / `CIDR`    | `String`                   | `@oid_inet`/`@oid_cidr` |
| `MACADDR`          | `String`                   | `@oid_macaddr` (829)    |

## Metadata via `sys.information.*`

Full reference: [../metadata\_sys\_information.md](../metadata_sys_information.md).

The lane exposes direct metadata query helpers (source: `lib/scratchbird/metadata.ex`):

```elixir
ScratchBird.Connection.schemas_query(conn)
ScratchBird.Connection.tables_query(conn)
ScratchBird.Connection.columns_query(conn)
ScratchBird.Connection.indexes_query(conn)
ScratchBird.Connection.primary_keys_query(conn)
ScratchBird.Connection.foreign_keys_query(conn)
ScratchBird.Connection.routines_query(conn)
ScratchBird.Connection.catalogs_query(conn)
ScratchBird.Connection.type_info_query(conn)
```

The remaining metadata gap is live-wire proof for restrictions and wildcards;
local query families are present.

## Errors and Diagnostics

Full reference: [../diagnostics\_and\_sqlstate.md](../diagnostics_and_sqlstate.md).

Error classification is handled by `ScratchBird.Errors` (source:
`lib/scratchbird/errors.ex`):

```elixir
ScratchBird.Errors.sqlstate_class("08006")  # => :connection_exception
ScratchBird.Errors.retry_scope("40001")     # => :statement
ScratchBird.Errors.retryable?("08001")      # => true
```

SQLSTATE class mapping covers: `:warning`, `:no_data`,
`:connection_exception`, `:feature_not_supported`, `:data_exception`,
`:integrity_constraint_violation`, `:invalid_authorization`,
`:transaction_rollback`, `:syntax_error_or_access_rule_violation`,
`:insufficient_resources`, `:program_limit_exceeded`,
`:operator_intervention`, `:system_error`, `:internal_error`.

## Pooling and Concurrency

Full reference: [../pooling\_and\_concurrency.md](../pooling_and_concurrency.md).

The driver is `thread_safe` with `session_pool` capability. Resilience
primitives (source: `lib/scratchbird/circuit_breaker.ex`, `keepalive.ex`,
`leak_detector.ex`, `telemetry.ex`):

- **Circuit breaker** — state-based failure/recovery tracking
- **Keepalive** — idle-window validation and periodic ping
- **Leak detector** — checkout tracking
- **Telemetry** — slow-query retention, metrics

Pooling uses DBConnection's built-in pool infrastructure. For external pool
configuration see ../pooling\_and\_concurrency.md.

## Conformance

Full reference: [../conformance\_baseline.md](../conformance_baseline.md).

Conformance gate: `driver_elixir_gate`. Groups and current status
(source: `BASELINE_REQUIREMENT_MAPPING.md`):

| JDBCBL group | Status      |
|--------------|-------------|
| `CONN`       | Implemented |
| `TXN`        | Implemented |
| `EXEC`       | Partial     |
| `META`       | Implemented |
| `TYPE`       | Implemented |
| `ERR`        | Implemented |
| `RES`        | Partial     |

Known open gap: deterministic stream/paging proof and live metadata
integration (local query families are complete).

## Platform Support

| Platform | Status    |
|----------|-----------|
| Linux    | Supported (CI) |
| Windows  | Supported (CI) |
| macOS    | Untested  |

## See Also

- [../README.md](../README.md) — Client & Driver Guide overview
- [../connection\_and\_dsn.md](../connection_and_dsn.md)
- [../authentication.md](../authentication.md)
- [../wire\_protocol\_sbwp.md](../wire_protocol_sbwp.md)
- [../type\_mapping.md](../type_mapping.md)
- [../metadata\_sys\_information.md](../metadata_sys_information.md)
- [../diagnostics\_and\_sqlstate.md](../diagnostics_and_sqlstate.md)
- [../pooling\_and\_concurrency.md](../pooling_and_concurrency.md)
- [../conformance\_baseline.md](../conformance_baseline.md)
