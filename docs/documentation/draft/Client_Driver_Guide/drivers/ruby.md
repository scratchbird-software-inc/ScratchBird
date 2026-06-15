# Ruby Driver — Native language binding for ScratchBird

The Ruby driver (`scratchbird` gem) is a native client for ScratchBird using the
ScratchBird wire protocol (SBWP v1.1). It exposes a `Scratchbird::Connection` class as
the primary application API and a lower-level `Scratchbird::Client` for protocol
interaction. The gem is pure Ruby; no native extensions are required.

Release status: **beta_2** (release_candidate gate). CI coverage on Linux and Windows.
macOS is not currently in CI.

---

## Driver metadata

| Field | Value |
|---|---|
| Driver package UUID | `019e12a0-0014-7000-8000-000000000014` |
| API surface | `language_binding` |
| Ingress modes | `direct_listener`, `manager_proxy` |
| Wire protocol | `sbwp_v1_1` |
| DSN keys | `database`, `host`, `port`, `user`, `auth_method` |
| Auth methods | `engine_local_password`, `scram_ready` |
| TLS profile | `scratchbird_tls_1_3_floor` |
| Type mapping profile | `sbsql_core` |
| Diagnostic mapping profile | `native_sqlstate` |
| Metadata profile | `sys_information_recursive` |
| Thread safety | `thread_safe` |
| Pooling capability | `connection_pool` |
| Conformance profile ref | `driver_ruby_gate` |

---

## Installation

Build and install from the gem specification (`scratchbird.gemspec`, version 0.1.0,
license MIT, requires Ruby >= 2.7):

```bash
gem build scratchbird.gemspec
gem install scratchbird-0.1.0.gem
```

Require in code:

```ruby
require "scratchbird"
```

---

## Connecting

### DSN forms

URI:
```
scratchbird://user:password@host:3092/database?sslmode=require
```

Key-value hash:
```ruby
{ host: "localhost", port: 3092, dbname: "mydb", user: "myuser", password: "mypass" }
```

### Minimal connection example

```ruby
require "scratchbird"

conn = Scratchbird.connect("scratchbird://user:pass@localhost:3092/mydb")
result = conn.query("SELECT 1 AS one")
puts result.first[0]
conn.close
```

`Scratchbird.connect` returns a `Scratchbird::Connection`. The lower-level
`Scratchbird::Client` is used internally and exposed for advanced use.

### Key DSN / Config parameters

| Key | Description |
|---|---|
| `host` | Server hostname |
| `port` | Default 3092 |
| `dbname` / `database` | Target database |
| `user` | Login user |
| `password` | Password |
| `auth_method` | `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`, `TOKEN` |
| `sslmode` | `require`, `verify-ca`, `verify-full` (disable is rejected) |
| `front_door_mode` | `direct_listener` or `manager_proxy` |
| `manager_auth_token` | Required when `front_door_mode=manager_proxy` |
| `auth_token` | Bearer token for TOKEN auth |

### Auth probe (staged bootstrap)

```ruby
# Module-level probe
surface = Scratchbird.probe_auth_surface("scratchbird://user@host:3092/mydb")

# Client-level probe
client = Scratchbird::Client.new(config)
surface = client.probe_auth_surface

# After connection
ctx = conn.resolved_auth_context
# or on a Client instance: client.get_resolved_auth_context
```

Admitted-but-unsupported methods (`MD5`, `PEER`, `REATTACH`) fail closed with SQLSTATE
`0A000`.

---

## Executing statements and transactions

### Query and execute

```ruby
# Simple query — returns Result object
result = conn.query("SELECT id, name FROM items")
result.each { |row| puts row.inspect }

# Parameterized (positional)
result = conn.query("SELECT id FROM items WHERE id = $1", [42])

# Execute (no rows returned)
conn.execute("DELETE FROM items WHERE id = $1", [99])
```

### Multi-result, batch, callable, generated keys

```ruby
# Multi-result
results = conn.query_multi("SELECT 1; SELECT 2")

# Batch execution
summary = conn.execute_batch("INSERT INTO t VALUES ($1)", [[1], [2], [3]])
# summary.items => Array<BatchItemSummary> (:index, :row_count, :command, :last_id)

# Generated keys
result = conn.execute_with_generated_keys(
  "INSERT INTO t(name) VALUES ($1) RETURNING id", ["Alice"]
)

# Callable (JDBC escape syntax)
conn.call("{ call my_proc(?) }", [arg])

# Callable SQL normalization
normalized = Scratchbird::Sql.normalize_callable("{ call routine(?) }")
```

### Prepared statements

```ruby
stmt = conn.prepare("SELECT id FROM items WHERE id = $1")
result = conn.execute_prepared(stmt, [42])
stmt.close
```

### Transaction control

```ruby
# Begin with MGA options
conn.begin_transaction(
  isolation_level: "READ COMMITTED",
  read_committed_mode: "READ COMMITTED READ CONSISTENCY",
  access_mode: "READ WRITE"
)

conn.savepoint("sp1")
conn.rollback_to_savepoint("sp1")
conn.release_savepoint("sp1")
conn.commit
# or:
conn.rollback
```

ScratchBird sessions are always in a transaction. `commit` / `rollback` immediately
reopen the next boundary. With `autocommit: false`, statements execute against the
server-owned session boundary (no synthetic client-side `BEGIN` is injected).

Isolation aliases: `READ COMMITTED` => canonical `READ COMMITTED`;
`REPEATABLE READ` => canonical `SNAPSHOT`; `SERIALIZABLE` => canonical
`SNAPSHOT TABLE STABILITY`.

`Scratchbird::Protocol.canonical_read_committed_mode_label(mode)` returns the
canonical label for auditing.

Retry scope:
```ruby
scope = Scratchbird::ErrorMapper.retry_scope_for_sqlstate("40001")
# :statement | :reconnect | :none
```

---

## Type mapping

The gem maps to the `sbsql_core` profile. Ruby types are decoded from wire OIDs
using `Scratchbird::Types`.

| SBSQL canonical type | Ruby class |
|---|---|
| BOOLEAN | `true` / `false` |
| SMALLINT / INTEGER / BIGINT | `Integer` |
| REAL / DOUBLE PRECISION | `Float` |
| NUMERIC / DECIMAL | `BigDecimal` |
| TEXT / VARCHAR | `String` |
| BYTEA | `String` (binary encoding) |
| DATE | `Date` |
| TIME | `Time` |
| TIMESTAMP | `Time` |
| TIMESTAMPTZ | `Time` |
| INTERVAL | `Hash` (`:micros`, `:days`, `:months`) |
| UUID | `String` |
| JSON | `Hash` / `Array` (parsed) |
| JSONB | `Scratchbird::JSONB` |
| ARRAY | `Array` |
| RANGE | `Scratchbird::RangeValue` |
| Geometry | `Scratchbird::Geometry` |

See [../type_mapping.md](../type_mapping.md) for the full type reference.

---

## Metadata and introspection

```ruby
# Metadata collection with optional restrictions
rows = conn.query_metadata_with_restrictions(
  "tables", { "schema" => "public" }
)

# Schema tree (with optional parent expansion)
tree = conn.get_schema_tree("my_schema",
  expand_schema_parents: true
)

# Restriction-aware schema tree
tree = conn.get_schema_with_restrictions(
  "schemas", { "schema" => "public" },
  expand_schema_parents: true
)
```

Supported collections: `schemas`, `tables`, `columns`, `indexes`, `index_columns`,
`constraints`, `primary_keys`, `foreign_keys`, `table_privileges`, `column_privileges`,
`procedures`, `functions`, `type_info`, `catalogs`, `routines`.

`Scratchbird::Metadata` provides `SchemaTreeNode`, `schema_paths_for_navigation`,
`build_schema_tree`, and `build_database_default_metadata_rows` for tree shaping.

See [../metadata_sys_information.md](../metadata_sys_information.md).

---

## Errors and diagnostics

Errors are raised as subclasses of `Scratchbird::Error`:

```ruby
begin
  conn.query("bad sql")
rescue Scratchbird::AuthorizationError => e
  puts "Auth failed: #{e.message}"
rescue Scratchbird::TransactionError => e
  scope = Scratchbird::ErrorMapper.retry_scope_for_sqlstate(e.sqlstate)
  retry if scope == :statement
rescue Scratchbird::Error => e
  puts "SQLSTATE=#{e.sqlstate}: #{e.message}"
end
```

`Scratchbird::Connection#supports_dormant_reattach?` returns `false`; dormant
detach/reattach methods fail closed until a public dormant token flow is available.

See [../diagnostics_and_sqlstate.md](../diagnostics_and_sqlstate.md).

---

## Resource resilience

`close` is idempotent — it finalizes keepalive unregistration and leak guard release
even when called from partial-startup or error paths. The `with_resilience` helper
provides circuit-breaker and telemetry accounting.

---

## Pooling and concurrency

The driver is rated `thread_safe`. The `connection_pool` capability is present.
Consult the application framework's connection pool (e.g., ActiveRecord connection
pool adapter) for pool management; the gem itself does not ship a standalone pool
class but is designed for safe use with external pools.

See [../pooling_and_concurrency.md](../pooling_and_concurrency.md).

---

## Conformance

This driver targets conformance profile `driver_ruby_gate`. JDBCBL baseline groups
CONN, TXN (Implemented), EXEC (Implemented), META (Partial — recursive tree shaping
present; full family and live integration coverage still expanding), TYPE, ERR, and
RES (Implemented) are covered. See the baseline mapping for remaining gaps.

See [../conformance_baseline.md](../conformance_baseline.md).

---

## See also

- [../README.md](../README.md) — Client and Driver Guide overview
- [../connection_and_dsn.md](../connection_and_dsn.md) — DSN reference
- [../authentication.md](../authentication.md) — Auth methods
- [../wire_protocol_sbwp.md](../wire_protocol_sbwp.md) — SBWP v1.1
