# ScratchBird Dart Driver — Language Binding

> **Status: beta\_2 / release\_candidate** — The Dart driver is production-capable on
> Linux and Windows (CI-verified). macOS is untested. All public APIs are stable for
> the SBWP v1.1 wire protocol. Use cautiously in production until the full
> release gate is cleared.

## Purpose

The Dart driver provides a native, async-first client for ScratchBird (CDE —
Convergent Data Engine) applications written in Dart or Flutter. It speaks
SBWP v1.1 directly over TCP/TLS and does not require any ODBC or JDBC layer.
The driver is a pure Dart package: no compiled native extensions are needed.

Target audience: Dart/Flutter developers building server-side or mobile
applications that need direct ScratchBird access.

## Manifest Metadata

| Field                    | Value                                      |
|--------------------------|--------------------------------------------|
| `driver_package_uuid`    | `019e12a0-0002-7000-8000-000000000002`     |
| `driver_family`          | `dart`                                     |
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
| `conformance_profile_ref`| `driver_dart_gate`                         |

## Install

The package name is `scratchbird` (see `pubspec.yaml`). Add it to your project:

```yaml
# pubspec.yaml
dependencies:
  scratchbird:
    path: /path/to/scratchbird   # local dev; pub.dev publication pending
```

Then fetch dependencies:

```bash
dart pub get
```

Runtime requirements: Dart SDK `>=3.2.0 <4.0.0`. Transitive dependencies:
`crypto: ^3.0.3`, `convert: ^3.1.1`.

## Connecting

The entry-point module is `package:scratchbird/scratchbird.dart`. The
primary public classes are `ScratchBirdConfig` and `ScratchBirdClient`.

**DSN form (URL):**

```
scratchbird://user:password@host:port/database[?option=value&...]
```

Default port: 3092. Optional DSN query parameters include `sslmode`
(`disable` | `allow` | `prefer` | `require` | `verify-ca` | `verify-full`),
`application_name`, `front_door_mode` (`direct_listener` | `manager_proxy`),
`manager_auth_token`, and standard timeout aliases (`connect_timeout`,
`socket_timeout`). See ../connection\_and\_dsn.md for the full key reference.

**Minimal connection example (verified against `lib/src/client.dart`):**

```dart
import 'package:scratchbird/scratchbird.dart';

Future<void> main() async {
  final config = ScratchBirdConfig.fromDsn(
    'scratchbird://user:pass@localhost:3092/mydb',
  );
  final client = await ScratchBirdClient.connect(config);

  final result = await client.query('SELECT 1');
  print(result.rows);

  await client.close();
}
```

### Manager-proxy ingress

For environments where connections pass through the ScratchBird manager
process, add `front_door_mode=manager_proxy` and supply `manager_auth_token`
in the DSN query string or config. See ../authentication.md.

### Auth discovery

Before committing credentials you can probe the front-door auth surface:

```dart
final surface = await ScratchBirdClient.probeAuthSurface(config);
final ctx     = await client.resolvedAuthContext();
```

Supported native auth methods: `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`,
`TOKEN`. Methods `MD5`, `PEER`, and `REATTACH` are admitted by the wire but
fail closed with a typed `ScratchBirdAuthException`.

## Executing Statements and Transactions

ScratchBird sessions are always in a transaction. `begin()` starts an
explicit transaction boundary; `commit()` and `rollback()` close it and
reopen the next boundary. There is no idle-session auto-begin.

```dart
// Simple query
final rows = await client.query('SELECT id, name FROM users WHERE active = $1',
    params: [true]);

// Explicit transaction
await client.begin(isolationLevel: 2); // SNAPSHOT (REPEATABLE READ alias)
try {
  await client.query('UPDATE counters SET n = n + 1 WHERE id = $1',
      params: [42]);
  await client.commit();
} catch (_) {
  await client.rollback();
  rethrow;
}

// Savepoints
await client.begin();
await client.savepoint('sp1');
// ... work ...
await client.releaseSavepoint('sp1');
await client.commit();
```

**Isolation-level aliases** (source: `lib/src/client.dart`):

| `isolationLevel` int | Wire canonical label             |
|----------------------|----------------------------------|
| `1` (`READ_COMMITTED`) | `READ COMMITTED`               |
| `2` (`REPEATABLE_READ`) | `SNAPSHOT`                    |
| `3` (`SERIALIZABLE`)  | `SNAPSHOT TABLE STABILITY`      |

The `READ COMMITTED` sub-mode is controlled by the `readCommittedMode`
parameter; use `canonicalReadCommittedModeLabel(mode)` to inspect the
canonical label at runtime.

**Retry guidance** (source: `lib/src/client.dart` — `retryScopeForSqlState`):

| SQLSTATE class | Retry boundary                 |
|----------------|--------------------------------|
| `40001`, `40P01` | Fresh statement only         |
| `08xxx`          | Reconnect / reopen only      |
| All others       | No automatic replay          |

### Prepared transactions and dormant sessions

`supportsPreparedTransactions()` → `true`; use `prepareTransaction(gid)`,
`commitPrepared(gid)`, `rollbackPrepared(gid)` for XA-style two-phase commit.
`supportsDormantReattach()` → `false`; `detachToDormant()` and
`reattachDormant()` fail closed with SQLSTATE `0A000` until the public front
door exposes the dormant token flow.

## Type Mapping

The driver uses OID-based binary and text wire encoding. Full type mapping
documentation: [../type\_mapping.md](../type_mapping.md).

| SBsql core type    | Dart type                | OID constant (src)      |
|--------------------|--------------------------|-------------------------|
| `BOOLEAN`          | `bool`                   | `oidBool` (16)          |
| `SMALLINT`         | `int`                    | `oidInt2` (21)          |
| `INTEGER`          | `int`                    | `oidInt4` (23)          |
| `BIGINT`           | `int`                    | `oidInt8` (20)          |
| `REAL`             | `double`                 | `oidFloat4` (700)       |
| `DOUBLE PRECISION` | `double`                 | `oidFloat8` (701)       |
| `NUMERIC`          | `String` (text form)     | `oidNumeric` (1700)     |
| `TEXT` / `VARCHAR` | `String`                 | `oidText`/`oidVarchar`  |
| `BYTEA`            | `Uint8List`              | `oidBytea` (17)         |
| `DATE`             | `DateTime`               | `oidDate` (1082)        |
| `TIME`             | `DateTime`               | `oidTime` (1083)        |
| `TIMESTAMP`        | `DateTime`               | `oidTimestamp` (1114)   |
| `TIMESTAMPTZ`      | `DateTime`               | `oidTimestamptz` (1184) |
| `UUID`             | `String`                 | `oidUuid` (2950)        |
| `JSON` / `JSONB`   | `String` / `Map`         | `oidJson`/`oidJsonb`    |
| `VECTOR`           | `List<double>`           | `oidVector` (16386)     |
| Arrays             | `List<T>`                | per element OID         |
| `COMPOSITE`        | `List<dynamic>`          | `oidRecord` (2249)      |
| Range types        | `Map` with bounds        | `oidInt4Range` etc.     |
| `INET` / `CIDR`    | `String`                 | `oidInet`/`oidCidr`     |
| `MACADDR`          | `String`                 | `oidMacaddr` (829)      |

## Metadata via `sys.information.*`

Full metadata reference: [../metadata\_sys\_information.md](../metadata_sys_information.md).

The client exposes typed metadata wrapper methods (source: `lib/src/client.dart`):

```dart
final schemas    = await client.metadataSchemas();
final tables     = await client.metadataTables();
final columns    = await client.metadataColumns();
final indexes    = await client.metadataIndexes();
final pks        = await client.metadataPrimaryKeys();
final fks        = await client.metadataForeignKeys();
final routines   = await client.metadataRoutines();
final typeInfo   = await client.metadataTypeInfo();
final schemaTree = await client.getSchemaTree();
```

Lower-level access uses `client.query(metadataQuery)` with the SQL constants
from `lib/src/metadata.dart` (`MetadataCollectionName`,
`resolveMetadataCollectionQuery(name)`).

## Errors and Diagnostics

Full diagnostics reference: [../diagnostics\_and\_sqlstate.md](../diagnostics_and_sqlstate.md).

Exception hierarchy (source: `lib/src/errors.dart`):

| Exception class                     | Typical SQLSTATE class   |
|-------------------------------------|--------------------------|
| `ScratchBirdException`              | base (all errors)        |
| `ScratchBirdConnectionException`    | `08xxx`                  |
| `ScratchBirdProtocolException`      | `08xxx`, framing errors  |
| `ScratchBirdAuthException`          | `28xxx`                  |
| `ScratchBirdTransactionException`   | `40xxx`, `25xxx`         |
| `ScratchBirdExecutionException`     | `42xxx`, `22xxx`         |
| `ScratchBirdOperationalException`   | `57xxx`, `54xxx`         |

Each exception exposes `.sqlState` (String?) and `.code` (int?).
Use `retryScopeForSqlState(sqlState)` to classify whether to retry at
statement or connection scope.

## Pooling and Concurrency

Full reference: [../pooling\_and\_concurrency.md](../pooling_and_concurrency.md).

The driver is `thread_safe` (Dart isolate-safe) with `session_pool`
capability. Built-in resilience primitives:

- `lib/src/circuit_breaker.dart` — state-based breaker (SQLSTATE `08006` on open)
- `lib/src/keepalive.dart` — idle-connection ping with configurable window
- `lib/src/leak_detector.dart` — checkout tracking with `onLeakDetected` hook
- `lib/src/telemetry.dart` — slow-query retention, metrics, tracing

Pipeline capacity is controlled by `pipeline_max_in_flight` DSN key; overflow
emits SQLSTATE `54000`.

## Conformance

Full conformance reference: [../conformance\_baseline.md](../conformance_baseline.md).

Conformance gate: `driver_dart_gate`. Groups and current status
(source: `BASELINE_REQUIREMENT_MAPPING.md`):

| JDBCBL group | Status      |
|--------------|-------------|
| `CONN`       | Implemented |
| `TXN`        | Partial     |
| `EXEC`       | Partial     |
| `META`       | Partial     |
| `TYPE`       | Partial     |
| `ERR`        | Partial     |
| `RES`        | Partial     |

Known open gaps: live integration coverage for pagination
(`portalSuspended`), SBLR execution, and complex binary type round-trips.

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
