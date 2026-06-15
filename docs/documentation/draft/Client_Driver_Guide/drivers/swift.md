# Swift Driver — async/await language binding for ScratchBird

The Swift driver (`ScratchBird` SwiftPM package) is a native async/await client for
ScratchBird. It speaks SBWP v1.1 over TCP/TLS using SwiftNIO and NIOSSL (or Apple
Network framework when no certificate files are supplied on Apple platforms). The
primary entry point is `ScratchBirdConnection`, created via
`ScratchBirdConnection.connect(_:)`. A lightweight `ScratchBirdConnectionPool` is also
provided.

Release status: **beta_2** (release_candidate gate). CI coverage on Linux. macOS is
expected to work with `swift build` but is not currently in CI. Windows is not
supported for this driver.

---

## Driver metadata

| Field | Value |
|---|---|
| Driver package UUID | `019e12a0-0016-7000-8000-000000000016` |
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
| Pooling capability | `session_pool` |
| Conformance profile ref | `driver_swift_gate` |

---

## Installation

SwiftPM package: name `ScratchBird`, swift-tools-version 5.10.1, license MPL-2.0.
Platforms: macOS 13+, iOS 16+.

`Package.swift` dependency:
```swift
.package(url: "https://github.com/scratchbird/scratchbird-swift.git", from: "0.1.0")
```

Add to your target:
```swift
.target(
    name: "MyApp",
    dependencies: [.product(name: "ScratchBird", package: "scratchbird-swift")]
)
```

Build from source:
```bash
swift build
```

The package depends on `swift-crypto`, `swift-nio`, and `swift-nio-ssl`.

---

## Connecting

### Minimal connection example

```swift
import ScratchBird

let config = ScratchBirdConfig(dsn: "scratchbird://user:pass@localhost:3092/mydb")
let conn = try await ScratchBirdConnection.connect(config)
let result = try await conn.query("SELECT 1")
print(result.rows)
try await conn.close()
```

### Struct-based config

```swift
let config = ScratchBirdConfig(
    host: "localhost",
    port: 3092,
    database: "mydb",
    user: "user",
    password: "pass",
    sslmode: "verify-full",
    sslrootcert: "/etc/ssl/certs/ca.pem"
)
```

### Key `ScratchBirdConfig` fields

| Field | Description |
|---|---|
| `host` | Server hostname |
| `port` | Default 3092 |
| `database` | Target database |
| `user` | Login user |
| `password` | Password |
| `sslmode` | `disable`, `allow`, `prefer`, `require`, `verify-ca`, `verify-full` |
| `sslrootcert` / `sslcert` / `sslkey` / `sslpassword` | Certificate paths (loaded via NIOSSL) |
| `front_door_mode` | `direct_listener` or `manager_proxy` |
| `manager_auth_token` | Required when `front_door_mode=manager_proxy` |
| `manager_username` / `manager_database` | Manager bootstrap fields |
| `auth_token` | Bearer token for TOKEN auth |
| `auth_method_id` / `auth_method_payload` | Auth method override |
| `auth_payload_json` / `auth_payload_b64` | Structured auth payloads |
| `workload_identity_token` | Workload identity token |
| `proxy_principal_assertion` | Proxy principal assertion |
| `keepalive_interval_ms` | Keepalive interval |
| `leak_detection_threshold_ms` | Leak detection threshold |

### Auth probe (staged bootstrap)

```swift
let probe = try await ScratchBirdConnection.probeAuthSurface(config)
// After connect:
let ctx = conn.getResolvedAuthContext()
```

Supported direct auth: `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`, `TOKEN`.
Admitted-but-unsupported methods (`MD5`, `PEER`, `REATTACH`) fail closed.

---

## Executing statements and transactions

### Query and execute

```swift
// Simple query
let result: ScratchBirdResult = try await conn.query("SELECT id, name FROM items")
for row in result.rows {
    print(row)
}

// Parameterized
let result2 = try await conn.query("SELECT id FROM items WHERE id = $1", [42])

// Execute (no rows)
let _ = try await conn.query("DELETE FROM items WHERE id = $1", [99])
```

### Batch, multi-statement, and first-column helpers

```swift
// Sequential batch execution
let batchResults = try await conn.executeBatch(
    "INSERT INTO t(name) VALUES ($1)",
    paramsBatch: [["Alice"], ["Bob"]]
)

// Multi-statement helper
let multiResults = try await conn.queryMulti(["SELECT 1", "SELECT 2"])

// First-column extraction (generated-key style)
let newId = try await conn.executeReturningFirstColumn(
    "INSERT INTO t(name) VALUES ($1) RETURNING id", ["Carol"]
)
```

### Transaction control

```swift
// Begin with MGA options
try await conn.begin(ScratchBirdTxnOptions(
    isolationLevel: .readCommitted,
    readCommittedMode: ScratchBirdReadCommittedMode.readConsistency,
    accessMode: .readWrite
))

try await conn.savepoint("sp1")
try await conn.rollbackToSavepoint("sp1")
try await conn.releaseSavepoint("sp1")
try await conn.commit()
// or:
try await conn.rollback()
```

ScratchBird sessions are always in a transaction. `commit()` / `rollback()` drain the
reopen boundary before returning. `begin(...)` adopts an already-active fresh native
boundary when using compatible default options; non-default fresh-boundary adoption
fails closed with `0A000`.

Isolation aliases: `readCommitted` => canonical `READ COMMITTED`;
`repeatableRead` => canonical `SNAPSHOT`; `serializable` => canonical
`SNAPSHOT TABLE STABILITY`.

`ScratchBirdReadCommittedMode.readConsistency` maps to canonical
`READ COMMITTED READ CONSISTENCY`.

Retry scope:
```swift
let scope = conn.retryScope(forSqlState: "40001")
// .statement | .reconnect | .none
let retryable = conn.isRetryable(sqlState: "40001")
```

Prepared and limbo control:
```swift
let supported = conn.supportsPreparedTransactions()
try await conn.prepareTransaction("txn_name")
try await conn.commitPrepared("txn_name")
try await conn.rollbackPrepared("txn_name")
```

Dormant transactions: `conn.supportsDormantReattach()` returns `false`;
`detachToDormant()` and `reattachDormant(...)` fail closed with `0A000`.

---

## Type mapping

The driver maps to the `sbsql_core` profile. Swift types used in `ScratchBirdResult`
rows and parameters:

| SBSQL canonical type | Swift type |
|---|---|
| BOOLEAN | `Bool` |
| SMALLINT / INTEGER / BIGINT | `Int` |
| REAL / DOUBLE PRECISION | `Double` |
| NUMERIC / DECIMAL | `String` (preserves precision) |
| TEXT / VARCHAR | `String` |
| BYTEA | `Data` |
| DATE | `String` (ISO 8601) |
| TIME | `String` (ISO 8601) |
| TIMESTAMP | `String` (ISO 8601) |
| TIMESTAMPTZ | `String` (ISO 8601 with timezone) |
| INTERVAL | `Interval` (struct: `micros`, `days`, `months`) |
| UUID | `String` |
| JSON | `Json` (struct: `raw: Data`) |
| JSONB | `Jsonb` (struct: `raw: Data`) |
| INET / CIDR | `ScratchBirdInet` / `ScratchBirdCidr` (structs) |
| ARRAY | `[Any?]` |
| RANGE | `ScratchBirdRange` (struct) |
| Geometry | `Geometry` (struct: `wkb: Data`) |

See [../type_mapping.md](../type_mapping.md) for the full type reference.

---

## Metadata and introspection

Connection-level metadata wrappers query `sys.*` catalog families:

```swift
let schemas   = try await conn.metadataSchemas()
let tables    = try await conn.metadataTables()
let columns   = try await conn.metadataColumns()
let indexes   = try await conn.metadataIndexes()
let pkeys     = try await conn.metadataPrimaryKeys()
let fkeys     = try await conn.metadataForeignKeys()
let typeInfo  = try await conn.metadataTypeInfo()
// also: metadataIndexColumns, metadataConstraints, metadataProcedures,
//       metadataFunctions, metadataRoutines, metadataCatalogs,
//       metadataTablePrivileges, metadataColumnPrivileges

// Recursive schema tree
let treeRows = try await conn.metadataSchemaTreeRows()
let tree     = try await conn.metadataSchemaTree()
```

`Metadata.swift` provides `buildMetadataSchemaTree`, `buildMetadataSchemaTreeRows`,
`metadataSchemaPathsForNavigation`, and `splitMetadataSchemaPath` for tree shaping
with optional parent expansion and per-parent uniqueness.

See [../metadata_sys_information.md](../metadata_sys_information.md).

---

## Errors and diagnostics

Wire errors are mapped to typed Swift exceptions by SQLSTATE class or exact code:

```swift
import ScratchBird

do {
    let _ = try await conn.query("bad sql")
} catch let e as ScratchBirdConnectionException {
    print("Connection error: \(e.sqlState ?? "?") \(e.localizedDescription)")
} catch let e as ScratchBirdTransactionException {
    let scope = conn.retryScope(forSqlState: e.sqlState ?? "")
    if scope == .statement { /* retry statement */ }
} catch let e as ScratchBirdProgrammingException {
    // syntax / parameter error
}
```

Typed exception classes:
`ScratchBirdConnectionException`, `ScratchBirdAuthorizationException`,
`ScratchBirdDataException`, `ScratchBirdIntegrityException`,
`ScratchBirdTransactionException`, `ScratchBirdProgrammingException`,
`ScratchBirdNotSupportedException`, `ScratchBirdTimeoutException`,
`ScratchBirdOperationalException`.

All carry `sqlState`, `severity`, `detail`, and `hint` fields and preserve
`NSError` compatibility via `errorUserInfo`.

See [../diagnostics_and_sqlstate.md](../diagnostics_and_sqlstate.md).

---

## Pooling and concurrency

```swift
let pool = ScratchBirdConnectionPool(config: config, maxSize: 4)

// Checkout / release
let conn = try await pool.acquire()
let result = try await conn.query("SELECT 1")
await pool.release(conn)

// Scoped helper
let result = try await pool.withConnection { conn in
    try await conn.query("SELECT 1")
}
await pool.close()
```

`ScratchBirdPoolStats` tracks active, idle, and total connections.

See [../pooling_and_concurrency.md](../pooling_and_concurrency.md).

---

## Conformance

This driver targets conformance profile `driver_swift_gate`. JDBCBL groups CONN and TXN
are `Implemented`. EXEC and META are `Partial` — API surfaces are present and unit-tested
but full live integration depth (cancellation timing, portal suspend/resume, live metadata
completeness) is still in progress. See the baseline mapping for remaining gaps.

See [../conformance_baseline.md](../conformance_baseline.md).

---

## See also

- [../README.md](../README.md) — Client and Driver Guide overview
- [../connection_and_dsn.md](../connection_and_dsn.md) — DSN reference
- [../authentication.md](../authentication.md) — Auth methods
- [../wire_protocol_sbwp.md](../wire_protocol_sbwp.md) — SBWP v1.1
