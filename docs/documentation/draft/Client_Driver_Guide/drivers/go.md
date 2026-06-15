# ScratchBird Go Driver — database/sql

The ScratchBird Go driver connects Go applications to a ScratchBird Convergent
Data Engine (CDE) through the standard `database/sql` interface. It speaks the
ScratchBird Native Wire Protocol (SBWP v1.1) directly and registers under the
driver name `"scratchbird"`.

**Release status: beta_2 (release_candidate gate)**

---

## Manifest metadata

| Field | Value |
|---|---|
| `driver_package_uuid` | `019e12a0-0005-7000-8000-000000000005` |
| `api_surface_set` | `database_sql` |
| `ingress_mode_set` | `direct_listener`, `manager_proxy` |
| `wire_protocol_set` | `sbwp_v1_1` |
| `dsn_key_set` | `database`, `host`, `port`, `user`, `auth_method` |
| `auth_method_set` | `engine_local_password`, `scram_ready` |
| `tls_profile_set` | `scratchbird_tls_1_3_floor` |
| `type_mapping_profile` | `sbsql_core` |
| `diagnostic_mapping_profile` | `native_sqlstate` |
| `metadata_profile` | `sys_information_recursive` |
| `thread_safety_class` | `thread_safe` |
| `pooling_capability` | `connection_pool` |
| `release_bucket` | `release_candidate` |
| `conformance_profile_ref` | `driver_go_gate` |

---

## Installation

```bash
go get github.com/scratchbird/scratchbird-go
```

Module path: `github.com/scratchbird/scratchbird-go`. Requires Go 1.19 or
later.

---

## Connecting

Import the driver package for its side-effect registration, then use
`database/sql` normally:

```go
import (
    "database/sql"
    _ "github.com/scratchbird/scratchbird-go"
)

db, err := sql.Open("scratchbird", "scratchbird://user:pass@localhost:3092/mydb")
if err != nil {
    log.Fatal(err)
}
defer db.Close()
```

### DSN forms

URI form:

```
scratchbird://user:password@host:3092/database?sslmode=require
```

Key-value form:

```
host=localhost port=3092 dbname=mydb user=myuser password=mypass
```

The `ParseConfig(dsn string)` function parses both forms into a `Config`
struct. When the DSN is empty, `defaultConfig()` provides defaults:
`host=localhost`, `port=3092`, `sslmode=require`, `binary_transfer=true`,
`compression=off`.

### Config struct key fields

| Field | Type | Default |
|---|---|---|
| `Host` | `string` | `"localhost"` |
| `Port` | `int` | `3092` |
| `Database` | `string` | |
| `User` | `string` | |
| `Password` | `string` | |
| `SSLMode` | `string` | `"require"` |
| `FrontDoorMode` | `string` | `"direct"` |
| `AuthToken` | `string` | |
| `AuthMethodID` | `string` | |
| `ManagerAuthToken` | `string` | |
| `BinaryTransfer` | `bool` | `true` |
| `Compression` | `string` | `"off"` |

Accepted `sslmode` values: `disable`, `require`, `verify-ca`, `verify-full`.
Accepted `compression` values: `off`, `zstd`.

### Using a Connector directly

```go
connector, err := new(scratchbird.Driver).OpenConnector(dsn)
db := sql.OpenDB(connector)
```

### Auth preflight probe

```go
import sb "github.com/scratchbird/scratchbird-go"

result, err := sb.ProbeAuthSurface(ctx, dsn)
// or on a Connector:
result, err := connector.ProbeAuthSurface(ctx)
// resolved auth after connect:
ctx2, err := conn.GetResolvedAuthContext()
```

Directly executable auth classes: `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`,
`TOKEN`. `MD5`, `PEER`, and `REATTACH` are fail-closed (`0A000`).

---

## Executing statements and transactions

### Query and exec

```go
rows, err := db.QueryContext(ctx, "SELECT id, name FROM users WHERE region = $1", "west")
defer rows.Close()
for rows.Next() {
    var id int64
    var name string
    rows.Scan(&id, &name)
}

result, err := db.ExecContext(ctx, "DELETE FROM sessions WHERE expires < $1", cutoff)
affected, _ := result.RowsAffected()
```

Parameter placeholder style is `$N` (positional).

### Multiple result sets

```go
rows, err := db.QueryContext(ctx, "SELECT 1; SELECT 2")
// consume first result set
rows.NextResultSet()
// consume second result set
```

### Transaction control

```go
tx, err := db.BeginTx(ctx, &sql.TxOptions{
    Isolation: sql.LevelReadCommitted,
    ReadOnly:  false,
})
tx.ExecContext(ctx, "UPDATE ...")
tx.Commit()
```

`BeginTx` exposes the standard `database/sql` isolation / read-only subset.
For the extended ScratchBird-specific options use `BeginTxEx`:

```go
// Extended form (driver-owned surface, not database/sql):
conn, _ := db.Conn(ctx)
defer conn.Close()
err = conn.Raw(func(rawConn any) error {
    sbconn := rawConn.(*scratchbird.Conn)
    return sbconn.BeginTxEx(ctx, scratchbird.TxOptions{
        Isolation:       sql.LevelReadCommitted,
        ReadCommittedMode: scratchbird.ReadCommittedModeReadConsistency,
    })
})
```

Isolation alias mapping:

| `sql.IsolationLevel` | Canonical MGA mode |
|---|---|
| `LevelReadUncommitted` | Legacy alias only |
| `LevelReadCommitted` | `READ COMMITTED` |
| `LevelRepeatableRead` | `SNAPSHOT` |
| `LevelSerializable` | `SNAPSHOT TABLE STABILITY` |

ScratchBird sessions are always in a transaction. `Commit` / `Rollback`
reopen the next boundary; the driver drains the reopen `READY` before the
next operation.

### Savepoints

```go
conn.Raw(func(rawConn any) error {
    sbconn := rawConn.(*scratchbird.Conn)
    sbconn.Savepoint(ctx, "sp1")
    sbconn.RollbackToSavepoint(ctx, "sp1")
    sbconn.ReleaseSavepoint(ctx, "sp1")
    return nil
})
```

### Prepared / limbo transaction control

```go
sbconn.PrepareTransaction(ctx, "xid1")
sbconn.CommitPrepared(ctx, "xid1")
sbconn.RollbackPrepared(ctx, "xid1")
```

`SupportsDormantReattach()` returns `false`; `DetachToDormant` and
`ReattachDormant` fail closed with `0A000`.

### Multi-result and batch convenience APIs (driver-owned surface)

```go
summaries, err := sbconn.QueryMultiContext(ctx, "SELECT 1; SELECT 2", nil)
batchSummary, err := sbconn.ExecuteBatchContext(ctx, "INSERT INTO t (x) VALUES ($1)", args)
keys, err := sbconn.ExecuteWithGeneratedKeys(ctx, "INSERT INTO orders (amount) VALUES ($1)", 99.5)
```

### Retry scope

```go
scope := scratchbird.RetryScopeForSQLState("40001") // "statement"
```

`40001`/`40P01` → `"statement"`; `08xxx` → `"reconnect"`; others → `"none"`.

### Pool reset

The `database/sql` pool calls `ResetSession` on checkout, which rolls back
any abandoned explicit transaction state and clears stale plan/SBLR borrow
caches.

---

## Type mapping

The `sbsql_core` profile applies. Go type mappings:

| SBsql canonical type | Go type (Scan target) |
|---|---|
| `BOOLEAN` | `bool` |
| `SMALLINT` (`INT2`) | `int16` |
| `INTEGER` (`INT4`) | `int32` |
| `BIGINT` (`INT8`) | `int64` |
| `REAL` (`FLOAT4`) | `float32` |
| `DOUBLE PRECISION` (`FLOAT8`) | `float64` |
| `NUMERIC` / `DECIMAL` | `string` or `[]byte` |
| `TEXT` / `VARCHAR` / `CHAR` | `string` |
| `BYTEA` | `[]byte` |
| `DATE` | `time.Time` |
| `TIME` | `time.Time` |
| `TIMETZ` | `time.Time` |
| `TIMESTAMP` | `time.Time` (naive) |
| `TIMESTAMPTZ` | `time.Time` (UTC) |
| `UUID` | `[16]byte` or `string` |
| `JSON` / `JSONB` | `[]byte` |
| `POINT` / geometry | `string` (text form) |
| array / range types | `[]byte` (raw wire form) |

OID constants are defined in `types.go` (e.g., `oidBool=16`, `oidTimetz=1266`,
`oidSBVector=16386`).

See [../type_mapping.md](../type_mapping.md).

---

## Metadata and introspection

```go
conn.Raw(func(rawConn any) error {
    sbconn := rawConn.(*scratchbird.Conn)
    rows, err := sbconn.QueryMetadata(ctx, "tables", nil)
    // or with restrictions:
    rows, err = sbconn.QueryMetadataWithRestrictions(ctx, "columns",
        map[string]string{"TABLE_SCHEMA": "public", "TABLE_NAME": "orders"})
    return err
})
```

`NormalizeMetadataCollectionName` normalizes collection aliases.
`ResolveMetadataCollectionQuery` resolves the backing `sys.information.*`
query. Restriction filtering supports null matching and unknown-key ignore.
The `MetadataExpandSchemaParents` config flag enables dotted parent expansion.

See [../metadata_sys_information.md](../metadata_sys_information.md).

---

## Errors and diagnostics

Errors implement `error` and may be cast to `*scratchbird.Error` to access:
- `SQLState` — five-character SQLSTATE string
- `Message` — primary message
- `Detail` / `Hint` — optional detail and hint fields

```go
var sbErr *scratchbird.Error
if errors.As(err, &sbErr) {
    fmt.Println(sbErr.SQLState, sbErr.Message)
}
```

See [../diagnostics_and_sqlstate.md](../diagnostics_and_sqlstate.md).

---

## Pooling and concurrency

The driver is `thread_safe`. `database/sql`'s built-in pool (`db.SetMaxOpenConns`,
`db.SetMaxIdleConns`, `db.SetConnMaxLifetime`) is the primary pooling surface.
The driver implements `driver.SessionResetter` so the pool calls `ResetSession`
on each checkout to roll back abandoned transactions and clear borrow caches.
Internal resilience helpers include `CircuitBreaker`, `KeepaliveManager`, and
`LeakDetector`.

See [../pooling_and_concurrency.md](../pooling_and_concurrency.md).

---

## Conformance

Conformance gate: `driver_go_gate`. All GOBL groups (CONN, TXN, EXEC, META,
TYPE, ERR, RES) are implemented per `BASELINE_REQUIREMENT_MAPPING.md`. A
manifest-driven conformance harness is in `conformance/harness.go`.

Run conformance tests:

```bash
export SCRATCHBIRD_GO_URL="scratchbird://user:pass@localhost:3092/mydb"
export SCRATCHBIRD_CONFORMANCE_MANIFEST="docs/fixtures/sbwp_conformance_manifest.json"
go test ./...
```

See [../conformance_baseline.md](../conformance_baseline.md).

---

## See also

- [../README.md](../README.md)
- [../connection_and_dsn.md](../connection_and_dsn.md)
- [../authentication.md](../authentication.md)
- [../wire_protocol_sbwp.md](../wire_protocol_sbwp.md)
