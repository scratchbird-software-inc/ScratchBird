# ScratchBird Pascal/Delphi Driver — Language Binding

> **Status: beta\_2 / release\_candidate** — The Pascal driver is supported on
> Linux and Windows (CI-verified via FreePascal). macOS is untested. The driver
> includes native TLS via OpenSSL and adapters for FireDAC, IBX, Zeos, and
> SQLdb. Use cautiously in production until the full release gate is cleared.

## Purpose

The Pascal/Delphi driver (`ScratchBird.Client`) provides native ScratchBird
wire-protocol access for Object Pascal applications compiled with FreePascal
(`fpc -Mdelphi`) or Delphi. It includes a standalone low-level client class
(`TScratchBirdClient`) and adapter shims for four major Pascal database
frameworks: FireDAC, IBX, Zeos, and SQLdb.

Target audience: Pascal/Delphi developers using RAD tools or writing server
applications who want a first-party ScratchBird client without a PostgreSQL
compatibility layer.

## Manifest Metadata

| Field                    | Value                                      |
|--------------------------|--------------------------------------------|
| `driver_package_uuid`    | `019e12a0-0010-7000-8000-000000000010`     |
| `driver_family`          | `pascal`                                   |
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
| `conformance_profile_ref`| `driver_pascal_gate`                       |

## Install / Build

The driver is compiled from source using FreePascal. Runtime TLS requires
`libssl` and `libcrypto` to be present on the target system.

**Linux / Windows (FreePascal):**

```bash
fpc -Mdelphi \
    -Fu./src \
    -FU./build \
    -FE./bin \
    YourApp.pas
```

Source units in `src/` include:
`ScratchBird.Client.pas`, `ScratchBird.Config.pas`,
`ScratchBird.Protocol.pas`, `ScratchBird.Types.pas`,
`ScratchBird.Metadata.pas`, `ScratchBird.Errors.pas`,
`ScratchBird.Transport.Native.pas`, `ScratchBird.Tls.Context.pas`,
`ScratchBird.Scram.pas`, `ScratchBird.AuthBootstrap.pas`,
`SBCircuitBreaker.pas`, `SBKeepalive.pas`, `SBLeakDetector.pas`,
`SBPipeline.pas`, `SBTelemetry.pas`.

Adapter units: `ScratchBird.FireDAC.pas`, `ScratchBird.IBX.pas`,
`ScratchBird.Zeos.pas`, `ScratchBird.SQLdb.pas`.

**Legacy Indy transport:** compile with `-dSCRATCHBIRD_USE_INDY` and add
vendored Indy paths (`third_party/indy/Lib/Core`, `Lib/Protocols`,
`Lib/System`, `Lib/Security`) if migration from Indy is still in progress.

## Connecting

The primary entry-point unit is `ScratchBird.Client`. The main class is
`TScratchBirdClient`.

**DSN form (URL):**

```
scratchbird://user:password@host:port/database[?option=value&...]
```

Default port: 3092. `sslmode` options: `disable`, `allow`, `prefer`,
`require`, `verify-ca`, `verify-full`. `sslmode=disable` maps to plaintext
socket mode. See ../connection\_and\_dsn.md for the full key reference.

**Minimal connection example (core client, verified against
`src/ScratchBird.Client.pas`):**

```pascal
uses
  ScratchBird.Client;

var
  Client: TScratchBirdClient;
begin
  Client := TScratchBirdClient.Create;
  try
    Client.Connect('scratchbird://user:pass@localhost:3092/mydb');
    Client.ExecSQL('SELECT 1');
  finally
    Client.Free;
  end;
end;
```

### Manager-proxy ingress

Manager-proxy mode requires `manager_auth_token` to be present in the DSN or
config before `Connect` is called; the client performs a fail-fast check
before any network dial. See ../authentication.md.

### Auth discovery

```pascal
var Surface: TAuthSurface;
    Ctx: TResolvedAuthContext;
begin
  Surface := TScratchBirdClient.ProbeAuthSurface(dsn);
  Ctx := Client.GetResolvedAuthContext();
end;
```

Supported native auth methods: `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`,
`TOKEN`. Methods `MD5`, `PEER`, and `REATTACH` are admitted by wire
negotiation but fail closed with typed auth errors.

## Executing Statements and Transactions

```pascal
// Simple execution
Client.ExecSQL('UPDATE counters SET n = n + 1 WHERE id = 1');

// Parameterized
Client.ExecSQL('SELECT * FROM users WHERE id = $1', [42]);

// Explicit transaction
Client.BeginTransactionEx(isoReadCommitted, amReadWrite);
try
  Client.ExecSQL('UPDATE ...');
  Client.Commit;
except
  Client.Rollback;
  raise;
end;

// Savepoint
Client.BeginTransactionEx(isoSnapshot, amReadWrite);
Client.Savepoint('sp1');
// ... work ...
Client.ReleaseSavepoint('sp1');
Client.Commit;
```

**`BeginTransactionEx` isolation options** (source:
`src/ScratchBird.Client.pas`, `src/ScratchBird.Protocol.pas`):

| Pascal constant      | Wire canonical label             |
|----------------------|----------------------------------|
| `isoReadUncommitted` | legacy compatibility alias       |
| `isoReadCommitted`   | `READ COMMITTED`                 |
| `isoRepeatableRead`  | `SNAPSHOT`                       |
| `isoSerializable`    | `SNAPSHOT TABLE STABILITY`       |

The `READ COMMITTED` sub-mode is set via the overloaded
`BeginTransactionEx(..., ReadCommittedMode)` or the adapter surface
`StartTransactionEx(..., ReadCommittedMode)`. Use
`CanonicalReadCommittedModeName(mode)` to retrieve the canonical string.

**Retry boundary** (source: `src/ScratchBird.Errors.pas` —
`RetryScopeForSqlState`):

| SQLSTATE         | Retry boundary                  |
|------------------|---------------------------------|
| `40001`, `40P01` | Fresh statement only            |
| `08xxx`          | Reconnect / reopen only         |
| All others       | No automatic replay             |

### Batch and multi-result execution

```pascal
// ExecuteBatch: per-statement summary output
Client.ExecuteBatch(['INSERT ...', 'UPDATE ...']);

// QueryMulti: per-statement rowset materialization
var Results: TQueryMultiResult;
Results := Client.QueryMulti(['SELECT ...', 'SELECT ...']);
```

### Generated keys

`TScratchBirdResultStream` exposes `LastInsertId` and `HasLastInsertId`
from the `MSG_COMMAND_COMPLETE` message when the engine returns a generated
key. Opt-in live verification requires the `SCRATCHBIRD_PASCAL_GENERATED_KEY_SQL`
environment variable (see integration tests).

### Prepared transactions and dormant sessions

`SupportsPreparedTransactions()` → `true`. Use `PrepareTransaction(gid)`,
`CommitPrepared(gid)`, `RollbackPrepared(gid)` for two-phase commit.
`SupportsDormantReattach()` → `false`; `DetachToDormant()` and
`ReattachDormant()` fail closed with SQLSTATE `0A000`.

### Portal resume

Result stream continuation (`TScratchBirdResultStream`) is gated on receiving
an explicit `MSG_PORTAL_SUSPENDED` state; blind resume attempts fail closed
with SQLSTATE `55000`.

## Adapter Usage

Each adapter unit wraps `TScratchBirdClient` under the standard framework
interface (source: `src/ScratchBird.FireDAC.pas` etc.):

```pascal
// FireDAC
uses ScratchBird.FireDAC;
var Conn: TSBFireDACConnection;
begin
  Conn := TSBFireDACConnection.Create(nil);
  Conn.ConnectionString := 'scratchbird://user:pass@localhost:3092/mydb';
  Conn.Connected := True;
end;

// IBX, Zeos, SQLdb follow the same pattern with their own adapter classes.
```

Transaction begin/commit/rollback are forwarded to `TScratchBirdClient`
through overridable execution hooks. The `StartTransactionEx` adapter method
exposes the full `ReadCommittedMode` parameter.

## Type Mapping

Full reference: [../type\_mapping.md](../type_mapping.md).

| SBsql core type       | Pascal type                  | OID constant (src)        |
|-----------------------|------------------------------|---------------------------|
| `BOOLEAN`             | `Boolean`                    | `OID_BOOL` (16)           |
| `SMALLINT`            | `SmallInt`                   | `OID_INT2` (21)           |
| `INTEGER`             | `Integer`                    | `OID_INT4` (23)           |
| `BIGINT`              | `Int64`                      | `OID_INT8` (20)           |
| `REAL`                | `Single`                     | `OID_FLOAT4` (700)        |
| `DOUBLE PRECISION`    | `Double`                     | `OID_FLOAT8` (701)        |
| `NUMERIC`             | `Currency` / `Extended`      | `OID_NUMERIC` (1700)      |
| `TEXT` / `VARCHAR`    | `String`                     | `OID_TEXT`/`OID_VARCHAR`  |
| `BYTEA`               | `TBytes`                     | `OID_BYTEA` (17)          |
| `DATE`                | `TDate`                      | `OID_DATE` (1082)         |
| `TIME`                | `TTime`                      | `OID_TIME` (1083)         |
| `TIMETZ`              | `TTimeTz` record             | `OID_TIMETZ` (1266)       |
| `TIMESTAMP`           | `TDateTime`                  | `OID_TIMESTAMP` (1114)    |
| `TIMESTAMPTZ`         | `TDateTime`                  | `OID_TIMESTAMPTZ` (1184)  |
| `INTERVAL`            | `TScratchBirdInterval`       | `OID_INTERVAL` (1186)     |
| `UUID`                | `String`                     | `OID_UUID` (2950)         |
| `JSON` / `JSONB`      | `String`                     | `OID_JSON`/`OID_JSONB`    |
| `VECTOR`              | `array of Double`            | `OID_SB_VECTOR` (16386)   |
| `INET` / `CIDR`       | `String`                     | `OID_INET`/`OID_CIDR`     |
| `MACADDR` / `MACADDR8`| `String`                     | `OID_MACADDR`/`OID_MACADDR8`|
| Geometry              | OID-typed wrapper            | `OID_POINT` (600) etc.    |
| `COMPOSITE`           | `TArray<Variant>` (guarded)  | `OID_RECORD` (2249)       |
| Range types           | map/record with bounds       | `OID_INT4RANGE` (3904) etc.|

Pascal-specific: `TIMETZ` decode handles both 12-byte and backward-compatible
8-byte payloads. Geometry wrappers (`TSBPoint`, `TSBLseg`, etc.) preserve the
OID on decode. Malformed composite frames produce null-materialized records
rather than raising.

## Metadata via `sys.information.*`

Full reference: [../metadata\_sys\_information.md](../metadata_sys_information.md).

Source unit: `src/ScratchBird.Metadata.pas`. API:

```pascal
// Stream API
var Stream: TScratchBirdResultStream;
Stream := Client.QueryMetadata('tables');

// Materialized rows
var Rows: TMetadataRows;
Rows := Client.QueryMetadataRows('tables', ['TABLE_SCHEM=public']);

// Typed wrappers
var Schemas: TMetadataRows;
Schemas := Client.Schemas;
Tables   := Client.Tables;
Columns  := Client.Columns;
// Also: Indexes, IndexColumns, Constraints, Routines, Catalogs,
//       PrimaryKeys, ForeignKeys, TablePrivileges, ColumnPrivileges, TypeInfo.

// Recursive schema tree
var Tree: TMetadataSchemaTree;
Tree := Client.BuildMetadataSchemaTree(expandParents := True);
```

Restriction filtering supports exact match (`=`), wildcard (`LIKE ... ESCAPE
'\'`), and null (`IS NULL`) predicates via `FilterMetadataRowsByRestrictions`.

## Errors and Diagnostics

Full reference: [../diagnostics\_and\_sqlstate.md](../diagnostics_and_sqlstate.md).

Source unit: `src/ScratchBird.Errors.pas`.

```pascal
var Cat: TScratchBirdErrorCategory;
Cat := MapSqlState('08006');       // Returns ecConnectionException
var Scope: TRetryScope;
Scope := RetryScopeForSqlState('40001');  // Returns rsStatement
var Retryable: Boolean;
Retryable := IsRetryableSqlState('40001'); // Returns True
```

Error categories: `ecWarning`, `ecNoData`, `ecConnectionException`,
`ecFeatureNotSupported`, `ecDataException`, `ecIntegrityConstraint`,
`ecInvalidAuthorization`, `ecTransactionRollback`,
`ecSyntaxErrorOrAccessRule`, `ecInsufficientResources`,
`ecProgramLimitExceeded`, `ecOperatorIntervention`, `ecSystemError`,
`ecInternalError`, `ecGeneric`.

## Pooling and Concurrency

Full reference: [../pooling\_and\_concurrency.md](../pooling_and_concurrency.md).

The driver is `thread_safe` with `session_pool` capability. Resilience
primitives (source: `src/SBCircuitBreaker.pas`, `src/SBKeepalive.pas`,
`src/SBLeakDetector.pas`, `src/SBTelemetry.pas`, `src/SBPipeline.pas`):

- **Circuit breaker** — state-based, half-open recovery
- **Keepalive** — idle-window tracking with `MarkActive`; pinger invocation
- **Leak detector** — checkout/checkin bookkeeping with background thread
- **Telemetry** — operation tracking and slow-query retention
- **Pipeline** — queued flush management

## Conformance

Full reference: [../conformance\_baseline.md](../conformance_baseline.md).

Conformance gate: `driver_pascal_gate`. Groups and current status
(source: `BASELINE_REQUIREMENT_MAPPING.md`):

| JDBCBL group | Status      |
|--------------|-------------|
| `CONN`       | Implemented |
| `TXN`        | Implemented |
| `EXEC`       | Implemented |
| `META`       | Implemented |
| `TYPE`       | Implemented |
| `ERR`        | Implemented |
| `RES`        | Implemented |

This is the most complete JDBCBL implementation among the batch C drivers.
Live integration checks remain environment-gated via `SCRATCHBIRD_PASCAL_URL`.

## Platform Support

| Platform | Status    |
|----------|-----------|
| Linux    | Supported (CI, `fpc` compile) |
| Windows  | Supported (CI, `fpc` compile) |
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
