# Rust Driver — Async language binding for ScratchBird

The Rust driver (`scratchbird` crate) is an async-native client for ScratchBird built on
Tokio. It speaks SBWP v1.1 over TCP/TLS using `rustls`, exposes a `Client` type with
`async/await` methods, and provides connection pooling, circuit breaking, keepalive, and
leak detection. It is the idiomatic choice for Rust applications connecting directly.

Release status: **beta_2** (release_candidate gate). CI coverage on Linux and Windows.
macOS is not currently in CI.

---

## Driver metadata

| Field | Value |
|---|---|
| Driver package UUID | `019e12a0-0015-7000-8000-000000000015` |
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
| Conformance profile ref | `driver_rust_gate` |

---

## Installation

Add to `Cargo.toml` (crate name: `scratchbird`, version 0.1.0, license MPL-2.0):

```toml
[dependencies]
scratchbird = "0.1"
tokio = { version = "1", features = ["rt-multi-thread", "macros"] }
```

The crate requires `tokio` as the async runtime. `rustls` is used for TLS; no native
OpenSSL dependency.

---

## Connecting

### DSN forms

URI:
```
scratchbird://user:password@host:3092/database?sslmode=require
```

Key-value:
```
host=localhost port=3092 dbname=mydb user=myuser password=mypass
```

### Minimal connection example

```rust
use scratchbird::{Client, Config};

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut client = Client::new(Config::from_dsn(
        "scratchbird://user:pass@localhost:3092/mydb"
    )?);
    client.connect().await?;
    let result = client.query("SELECT 1").await?;
    println!("{:?}", result.rows[0][0]);
    client.close().await;
    Ok(())
}
```

### Key DSN / Config fields

| Key | Description |
|---|---|
| `host` | Server hostname |
| `port` | Default 3092 |
| `dbname` / `database` | Target database |
| `user` | Login user |
| `password` | Password |
| `auth_method` | `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`, `TOKEN` |
| `sslmode` | `disable`, `require`, `verify-ca`, `verify-full` |
| `front_door_mode` | `direct_listener` or `manager_proxy` |
| `manager_auth_token` | Required when `front_door_mode=manager_proxy` |
| `auth_token` | Bearer token for TOKEN auth |
| `auth_payload_json` / `auth_payload_b64` | Structured auth payloads |
| `workload_identity_token` | Workload identity token |
| `proxy_principal_assertion` | Proxy principal assertion |
| `compression` | `off` (default) or `zstd` |
| `binary_transfer` | `true` (default) or `false` |
| `metadata_expand_schema_parents` | Enable recursive parent expansion |

### Auth probe (staged bootstrap)

```rust
use scratchbird::probe_auth_surface;

let probe = probe_auth_surface("scratchbird://user@host:3092/mydb").await?;
// or on an existing client: client.probe_auth_surface().await?
let ctx = client.get_resolved_auth_context();
```

Admitted-but-unsupported methods (`MD5`, `PEER`, `REATTACH`) fail closed with SQLSTATE
`0A000`.

---

## Executing statements and transactions

### Query and execute

```rust
// Simple query
let result: QueryResult = client.query("SELECT id, name FROM items").await?;

// Parameterized query
use scratchbird::Params;
let result = client.query_params(
    "SELECT id FROM items WHERE id = $1",
    Params::positional(vec![42.into()])
).await?;

// Execute (no rows)
client.execute("DELETE FROM items WHERE id = $1",
    Params::positional(vec![99.into()])
).await?;
```

### Multi-result, batch, and callable

```rust
// Multi-result
let results = client.query_multi("SELECT 1; SELECT 2").await?;

// Batch execution
let summaries: BatchSummary = client.execute_batch(
    "INSERT INTO t VALUES ($1)", batch_params
).await?;

// Generated keys
let keys = client.execute_with_generated_keys(
    "INSERT INTO t(name) VALUES ($1) RETURNING id", params
).await?;

// Callable normalization
let sql = scratchbird::normalize_callable("{ call my_proc(?) }")?;
let result = client.call(&sql, params).await?;
```

### Transaction control

```rust
use scratchbird::protocol::{
    READ_COMMITTED_MODE_READ_CONSISTENCY,
    canonical_read_committed_mode_label,
};

// Begin with explicit options
client.begin(scratchbird::TxnBeginOptions {
    isolation_level: Some("READ COMMITTED".to_string()),
    read_committed_mode: Some(READ_COMMITTED_MODE_READ_CONSISTENCY),
    access_mode: Some("READ WRITE".to_string()),
    ..Default::default()
}).await?;

client.savepoint("sp1").await?;
client.rollback_to_savepoint("sp1").await?;
client.release_savepoint("sp1").await?;
client.commit().await?;
// or:
client.rollback().await?;
```

ScratchBird sessions are always in a transaction. `commit()` / `rollback()` immediately
reopen the next boundary. `begin()` restarts the current boundary with the requested
options rather than assuming idle-session semantics.

Isolation aliases: `READ COMMITTED` => canonical `READ COMMITTED`;
`REPEATABLE READ` => canonical `SNAPSHOT`; `SERIALIZABLE` => canonical
`SNAPSHOT TABLE STABILITY`.

Autocommit:
```rust
client.set_autocommit(true).await?;   // commits active txn, then switches mode
client.set_autocommit(false).await?;  // eagerly begins new txn if none active
```

Retry scope:
```rust
use scratchbird::retry_scope_for_sqlstate;
let scope = retry_scope_for_sqlstate("40001"); // RetryScope::Statement
```

---

## Type mapping

The crate maps to the `sbsql_core` profile. Types are exchanged via the `Value` and
`Param` enums, with concrete structs for complex types.

| SBSQL canonical type | Rust type |
|---|---|
| BOOLEAN | `bool` |
| SMALLINT | `i16` |
| INTEGER | `i32` |
| BIGINT | `i64` |
| REAL | `f32` |
| DOUBLE PRECISION | `f64` |
| NUMERIC / DECIMAL | `Decimal` (struct) |
| TEXT / VARCHAR | `String` |
| BYTEA | `Vec<u8>` |
| DATE | `Date` (struct) |
| TIME | `Time` (struct) |
| TIMESTAMP | `Timestamp` (struct) |
| TIMESTAMPTZ | `TimestampTz` (struct) |
| INTERVAL | `Interval` (struct) |
| UUID | `String` |
| JSON | `Json` (struct) |
| JSONB | `Jsonb` (struct) |
| MONEY | `Money` (struct) |
| ARRAY | `Value::Array` |
| RANGE | `Range<T>` (struct) |
| Geometry | `Geometry` (struct) |

See [../type_mapping.md](../type_mapping.md) for the full type reference.

---

## Metadata and introspection

```rust
// Named collection
let schemas = client.get_schema("schemas", None).await?;

// With restrictions
let tables = client.get_schema(
    "tables",
    Some(vec![("schema", "public")].into_iter().collect())
).await?;

// Recursive schema tree
let tree = client.get_schema_tree("my_schema", true).await?;

// DDL editor payload
let payload = client.ddl_editor_schema_payload("my_schema", true).await?;
```

Supported collections include `schemas`, `tables`, `columns`, `indexes`,
`index_columns`, `constraints`, `primary_keys`, `foreign_keys`, `table_privileges`,
`column_privileges`, `procedures`, `functions`, `routines`, `type_info`, `catalogs`.

See [../metadata_sys_information.md](../metadata_sys_information.md).

---

## Errors and diagnostics

Errors are returned as `scratchbird::Error` with a `kind: ErrorKind` discriminant:

```rust
use scratchbird::{Error, ErrorKind};

match result {
    Err(e) if e.kind() == ErrorKind::TxnConflict => { /* retry */ }
    Err(e) => eprintln!("SQLSTATE={:?}: {}", e.sqlstate(), e),
    Ok(r) => { /* use r */ }
}
```

`scratchbird::is_retryable_sqlstate(s)` and `retry_scope_for_sqlstate(s)` classify
SQLSTATEs for retry logic without hard-coding error codes.

See [../diagnostics_and_sqlstate.md](../diagnostics_and_sqlstate.md).

---

## Pooling and concurrency

`ConnectionPool` / `PooledConnection` wrap `Client` for bounded reuse:

```rust
use scratchbird::pool::{ConnectionPool, PoolConfig};

let pool = ConnectionPool::new(config, PoolConfig { max_size: 10, ..Default::default() });
let conn = pool.get().await?;
conn.query("SELECT 1").await?;
// conn returns to pool on drop
```

`with_retry(pool, config, op)` is a convenience wrapper for retry-on-conflict.

See [../pooling_and_concurrency.md](../pooling_and_concurrency.md).

---

## Conformance

This driver targets conformance profile `driver_rust_gate`. All seven JDBCBL capability
groups (CONN, TXN, EXEC, META, TYPE, ERR, RES) are status `Implemented` as of the current
baseline.

See [../conformance_baseline.md](../conformance_baseline.md).

---

## See also

- [../README.md](../README.md) — Client and Driver Guide overview
- [../connection_and_dsn.md](../connection_and_dsn.md) — DSN reference
- [../authentication.md](../authentication.md) — Auth methods
- [../wire_protocol_sbwp.md](../wire_protocol_sbwp.md) — SBWP v1.1
