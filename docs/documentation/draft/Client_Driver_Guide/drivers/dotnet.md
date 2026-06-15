# ScratchBird .NET Driver — ADO.NET

The ScratchBird .NET driver connects .NET applications to a ScratchBird
Convergent Data Engine (CDE) through the standard ADO.NET provider interfaces
(`DbConnection`, `DbCommand`, `DbDataReader`, `DbTransaction`). It speaks the
ScratchBird Native Wire Protocol (SBWP v1.1) directly. The NuGet package id is
`ScratchBird.Data`.

**Release status: beta_2 (release_candidate gate)**

---

## Manifest metadata

| Field | Value |
|---|---|
| `driver_package_uuid` | `019e12a0-0003-7000-8000-000000000003` |
| `api_surface_set` | `ado_net` |
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
| `conformance_profile_ref` | `driver_dotnet_gate` |

---

## Installation

The package is distributed as `ScratchBird.Data` (NuGet). Build from source
with the .NET 8 SDK:

```bash
dotnet build src/ScratchBird.Data/ScratchBird.Data.csproj
```

The `.csproj` specifies `TargetFramework=net8.0`, `PackageId=ScratchBird.Data`,
version `0.1.0`, license `Apache-2.0`.

Integration test environment variable: `SCRATCHBIRD_DOTNET_URL`. When unset,
live suites fall back to
`scratchbird://sb_admin:SbAdmin_Compat1!@127.0.0.1:13092/main?sslmode=disable&allow_insecure=true`.

---

## Connecting

### Connection string form

```
Host=db.example.com;Port=3092;Database=prod;Username=alice;Password=secret;SslMode=require
```

Or using the URI form:

```
scratchbird://alice:secret@db.example.com:3092/prod?sslmode=require
```

### Minimal connection example

```csharp
using ScratchBird.Data;

await using var conn = new ScratchBirdConnection(
    "Host=db.example.com;Port=3092;Database=prod;Username=alice;Password=secret;SslMode=require"
);
await conn.OpenAsync();
```

### Connection string builder

```csharp
var builder = new ScratchBirdConnectionStringBuilder
{
    Host = "db.example.com",
    Port = 3092,
    Database = "prod",
    Username = "alice",
    Password = "secret",
    SslMode = "require",
    FrontDoorMode = "direct",      // or "manager_proxy"
    AuthToken = "",                 // bearer token for TOKEN auth
    ManagerAuthToken = "",          // required when FrontDoorMode=manager_proxy
};
string connStr = builder.ToString();
```

### `ScratchBirdConfig` key properties

| Property | Default |
|---|---|
| `Host` | `"localhost"` |
| `Port` | `3092` |
| `Database` | `""` |
| `Username` | `""` |
| `Password` | `""` |
| `SslMode` | `"require"` |
| `FrontDoorMode` | `"direct"` |
| `AuthToken` | `""` |
| `AuthMethodId` | `""` |
| `ManagerAuthToken` | `""` |
| `BinaryTransfer` | `true` |
| `Compression` | `"off"` |

### Auth preflight probe

```csharp
var probe = await ScratchBirdConnection.ProbeAuthSurface(connectionString);
// resolved auth after connect:
var ctx = conn.GetResolvedAuthContext();
```

Directly executable auth classes: `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`,
`TOKEN`. `MD5`, `PEER`, and `REATTACH` are fail-closed (`0A000`).

Dormant reattach is supported on this driver: `SupportsDormantReattach()`
returns `true`. `DetachToDormant()` returns the engine-issued `dormant_id` and
`dormant_reattach_token`; `ReattachDormant(dormantId, token)` uses them at
reconnect.

---

## Executing statements and transactions

### Command execution

```csharp
await using var cmd = conn.CreateCommand();
cmd.CommandText = "SELECT id, name FROM users WHERE region = $1";
cmd.Parameters.Add(new ScratchBirdParameter { Value = "west" });

await using var reader = await cmd.ExecuteReaderAsync();
while (await reader.ReadAsync())
{
    long id = reader.GetInt64(0);
    string name = reader.GetString(1);
}
```

### Prepared commands

```csharp
await cmd.PrepareAsync();     // sends PARSE to the server
await cmd.ExecuteReaderAsync();
```

### Transaction control

```csharp
await using var tx = await conn.BeginTransactionAsync(
    System.Data.IsolationLevel.ReadCommitted
);
// ...
await tx.CommitAsync();
await tx.RollbackAsync();
```

Extended options via `ScratchBirdTransactionOptions`:

```csharp
await using var tx = await conn.BeginTransaction(
    new ScratchBirdTransactionOptions
    {
        IsolationLevel = System.Data.IsolationLevel.ReadCommitted,
        ReadCommittedMode = ScratchBirdReadCommittedMode.ReadConsistency,
        AccessMode = "read write",
        Deferrable = false,
        Wait = true,
        TimeoutMs = 5000,
        AutoCommit = false,
    }
);
```

### Isolation level mapping

| `System.Data.IsolationLevel` | Canonical MGA mode |
|---|---|
| `ReadUncommitted` | Legacy alias only |
| `ReadCommitted` | `READ COMMITTED` |
| `RepeatableRead` | `SNAPSHOT` |
| `Serializable` / `Snapshot` / `Chaos` | `SNAPSHOT TABLE STABILITY` |

`ScratchBirdReadCommittedMode.ReadConsistency` selects canonical
`READ COMMITTED READ CONSISTENCY`.

ScratchBird sessions are always in a transaction. `CommitAsync` /
`RollbackAsync` reopen the next boundary.

### Savepoints

```csharp
await conn.SaveAsync("sp1");
await conn.RollbackAsync("sp1");
await conn.ReleaseAsync("sp1");
```

### Prepared / limbo transaction control

```csharp
await conn.PrepareTransaction("xid1");
await conn.CommitPrepared("xid1");
await conn.RollbackPrepared("xid1");
```

### Multi-result and batch convenience APIs

```csharp
// Multi-result traversal:
var results = await conn.QueryMulti("SELECT 1; SELECT 2", null);

// Batch execution with summaries:
var summary = await conn.ExecuteBatch(
    "INSERT INTO t (x) VALUES ($1)",
    new[] { new[] { (object)1 }, new[] { (object)2 } }
);

// Generated keys:
var keys = await conn.ExecuteWithGeneratedKeys(
    "INSERT INTO orders (amount) VALUES ($1)", 99.5m
);
```

### Retry scope

```csharp
var scope = ScratchBirdSqlStateMapper.RetryScopeForSqlState("40001");
// "statement"
bool retryable = ScratchBirdSqlStateMapper.IsRetryableSqlState("40001");
```

---

## Type mapping

The `sbsql_core` profile applies. Key ADO.NET type mappings:

| SBsql canonical type | .NET type | `ScratchBirdDataReader.Get*` method |
|---|---|---|
| `BOOLEAN` | `bool` | `GetBoolean` |
| `SMALLINT` | `short` | `GetInt16` |
| `INTEGER` | `int` | `GetInt32` |
| `BIGINT` | `long` | `GetInt64` |
| `REAL` | `float` | `GetFloat` |
| `DOUBLE PRECISION` | `double` | `GetDouble` |
| `NUMERIC` / `DECIMAL` | `decimal` | `GetDecimal` |
| `TEXT` / `VARCHAR` | `string` | `GetString` |
| `BYTEA` | `byte[]` | `GetValue` |
| `DATE` | `DateOnly` | `GetDateOnly` |
| `TIME` | `TimeOnly` | `GetTimeOnly` |
| `TIMETZ` | `DateTimeOffset` | `GetDateTimeOffset` |
| `TIMESTAMP` | `DateTime` (unspecified kind) | `GetDateTime` |
| `TIMESTAMPTZ` | `DateTime` (UTC) | `GetDateTime` |
| `UUID` | `Guid` | `GetGuid` |
| `JSON` / `JSONB` | `string` | `GetString` |
| `INET` / `CIDR` | `string` | `GetString` |
| `MACADDR` / `MACADDR8` | `string` | `GetString` |
| array / range types | `object` | `GetValue` |

Decoded by `TypeDecoder`. Parameter binding uses `ScratchBirdParameter` with
`Value` set to the appropriate .NET type.

See [../type_mapping.md](../type_mapping.md).

---

## Metadata and introspection

```csharp
DataTable tables = conn.GetSchema("Tables",
    new[] { null, "public", null, null });
DataTable cols = conn.GetSchema("Columns",
    new[] { null, "public", "orders", null });
DataTable pks = conn.GetSchema("PrimaryKeys",
    new[] { null, "public", "orders" });
```

Supported collections: `Tables`, `Columns`, `Schemas`, `Catalogs`, `Indexes`,
`IndexColumns`, `Constraints`, `PrimaryKeys`, `ForeignKeys`, `TablePrivileges`,
`ColumnPrivileges`, `Procedures`, `Functions`, `Routines`, `TypeInfo`.
Restrictions support `"null"` literal matching and `%`/`_` wildcard patterns.

Schema-parent expansion:

```
Host=...;MetadataExpandSchemaParents=true
```

(aliases: `metadataExpandSchemaParents`, `metadata_expand_schema_parents`,
`expandSchemaParents`, `expand_schema_parents`, `dbeaverExpandSchemaParents`).

See [../metadata_sys_information.md](../metadata_sys_information.md).

---

## Errors and diagnostics

Errors are raised as `ScratchBirdException` (subclass of `DbException`):

```csharp
try
{
    await cmd.ExecuteNonQueryAsync();
}
catch (ScratchBirdException ex)
{
    Console.WriteLine(ex.SqlState);   // e.g., "23505"
    Console.WriteLine(ex.Message);
}
```

SQLSTATE codes are in `ex.SqlState`. Retry-boundary helpers:
`ScratchBirdSqlStateMapper.RetryScopeForSqlState(sqlstate)` and
`IsRetryableSqlState(sqlstate)`.

See [../diagnostics_and_sqlstate.md](../diagnostics_and_sqlstate.md).

---

## Pooling and concurrency

The driver is `thread_safe`. `ProtocolClientPool` provides internal connection
pooling with lease/acquire timeout, lifetime eviction, saturation fallback, and
cancellation cleanup. Set `Pooling=true` in the connection string and tune with
`MinPoolSize`, `MaxPoolSize`, `ConnectionLifetime`, and `PoolAcquireTimeoutMs`.

```
Host=...;Pooling=true;MaxPoolSize=20;ConnectionLifetime=300
```

See [../pooling_and_concurrency.md](../pooling_and_concurrency.md).

---

## Conformance

Conformance gate: `driver_dotnet_gate`. All DOTNETBL groups (CONN, TXN, EXEC,
META, TYPE, ERR, RES) are implemented per `BASELINE_REQUIREMENT_MAPPING.md`.
TXN full live isolation-matrix validation and output-parameter semantics remain
pending.

See [../conformance_baseline.md](../conformance_baseline.md).

---

## See also

- [../README.md](../README.md)
- [../connection_and_dsn.md](../connection_and_dsn.md)
- [../authentication.md](../authentication.md)
- [../wire_protocol_sbwp.md](../wire_protocol_sbwp.md)
