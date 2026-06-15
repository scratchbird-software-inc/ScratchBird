# PHP Driver — PDO-style language binding for ScratchBird

The PHP driver (`scratchbird/pdo-scratchbird` Composer package) is a pure-PHP PDO-style
client for ScratchBird. It speaks SBWP v1.1 over TCP/TLS directly from PHP — no
extensions or native libraries are required. The primary entry points are
`ScratchBird\PDO\ScratchBirdPDO` (a PDO-compatible facade) and
`ScratchBird\Connection` (the lower-level driver connection). The driver is
`connection_thread_confined`, meaning each connection must be used from a single
PHP request/process thread.

Release status: **beta_2** (release_candidate gate). CI coverage on Linux and Windows.
macOS is not currently in CI.

---

## Driver metadata

| Field | Value |
|---|---|
| Driver package UUID | `019e12a0-0011-7000-8000-000000000011` |
| API surface | `language_binding` |
| Ingress modes | `direct_listener`, `manager_proxy` |
| Wire protocol | `sbwp_v1_1` |
| DSN keys | `database`, `host`, `port`, `user`, `auth_method` |
| Auth methods | `engine_local_password`, `scram_ready` |
| TLS profile | `scratchbird_tls_1_3_floor` |
| Type mapping profile | `sbsql_core` |
| Diagnostic mapping profile | `native_sqlstate` |
| Metadata profile | `sys_information_recursive` |
| Thread safety | `connection_thread_confined` |
| Pooling capability | `session_pool` |
| Conformance profile ref | `driver_php_gate` |

---

## Installation

Composer package: `scratchbird/pdo-scratchbird`, version 0.1.0, license Apache-2.0.
Requires PHP >= 8.1.

```bash
composer require scratchbird/pdo-scratchbird
```

Autoload namespace: `ScratchBird\` and `ScratchBird\PDO\` map to `src/`.

---

## Connecting

### DSN forms

URI:
```
scratchbird://user:password@host:3092/database?sslmode=require
```

Key-value string:
```
host=localhost port=3092 dbname=mydb user=myuser password=mypass
```

### PDO-style connection (recommended)

```php
use ScratchBird\PDO\ScratchBirdPDO;

$pdo = new ScratchBirdPDO("scratchbird://user:pass@localhost:3092/mydb");
$stmt = $pdo->query("SELECT 1");
$row = $stmt->fetch();
```

The constructor signature is:
```php
ScratchBirdPDO(string $dsn, ?string $username = null, ?string $password = null, array $options = [])
```

### Lower-level `Connection`

```php
use ScratchBird\Connection;

$conn = new Connection("scratchbird://user:pass@localhost:3092/mydb");
$stream = $conn->query("SELECT id FROM items");
foreach ($stream as $row) {
    echo $row['id'] . "\n";
}
$conn->close();
```

### Key DSN parameters

| Key | Description |
|---|---|
| `host` | Server hostname |
| `port` | Default 3092 |
| `dbname` / `database` | Target database |
| `user` | Login user |
| `password` | Password |
| `auth_method` | `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`, `TOKEN` |
| `sslmode` | `require`, `verify-ca`, `verify-full` |
| `front_door_mode` | `direct_listener` or `manager_proxy` |
| `manager_auth_token` | Required when `front_door_mode=manager_proxy` |
| `auth_token` | Bearer token for TOKEN auth |
| `auth_payload_json` / `auth_payload_b64` | Structured auth payloads |
| `workload_identity_token` | Workload identity token |
| `proxy_principal_assertion` | Proxy principal assertion |

### Auth probe (staged bootstrap)

```php
use ScratchBird\Connection;

$surface = Connection::probeAuthSurface("scratchbird://user@host:3092/mydb");
// After connect:
$ctx = $conn->getResolvedAuthContext();
```

Admitted-but-unsupported methods (`MD5`, `PEER`, `REATTACH`) fail closed with SQLSTATE
`0A000`.

---

## Executing statements and transactions

### Query and execute via PDO

```php
// Query returning rows
$stmt = $pdo->query("SELECT id, name FROM items");
while ($row = $stmt->fetch(\PDO::FETCH_ASSOC)) {
    echo $row['name'] . "\n";
}

// Prepared statement
$stmt = $pdo->prepare("SELECT id FROM items WHERE id = ?");
$stmt->execute([42]);
$row = $stmt->fetch();

// Execute (no rows)
$affected = $pdo->exec("DELETE FROM items WHERE id = 99");
```

### Extended execution via `Connection`

```php
// Multi-result
$results = $conn->queryMulti("SELECT 1; SELECT 2");

// Batch
$summary = $conn->executeBatch("INSERT INTO t VALUES (?)", [[1], [2], [3]]);
// $summary->totalRowCount, $summary->items[0]->rowCount etc.

// Generated keys
$result = $conn->executeWithGeneratedKeys(
    "INSERT INTO t(name) VALUES (?) RETURNING id", ["Alice"]
);
$id = $conn->lastInsertId();

// Callable (JDBC escape syntax)
$conn->call("{ call my_proc(?) }", [$arg]);

// Native SQL normalization
$sql  = $conn->nativeSql("SELECT * FROM t WHERE id = ?");
$csql = $conn->nativeCallableSql("{ call proc(?) }");
```

### Statement traversal across multi-result

```php
$stmt = $pdo->query("SELECT 1; SELECT 2");
do {
    while ($row = $stmt->fetch()) { /* process */ }
} while ($stmt->nextRowset());
```

### Transaction control

```php
// Standard PDO transaction
$pdo->beginTransaction();
$pdo->exec("INSERT INTO t VALUES (1)");
$pdo->commit();
// or $pdo->rollBack();
echo $pdo->inTransaction() ? "in txn" : "no txn";

// Extended begin with MGA options
$conn->beginTransactionEx([
    'isolation_level'     => 'READ COMMITTED',
    'read_committed_mode' => 'READ COMMITTED READ CONSISTENCY',
    'access_mode'         => 'READ WRITE',
]);
$conn->savepoint("sp1");
$conn->rollbackToSavepoint("sp1");
$conn->releaseSavepoint("sp1");
$conn->commit();
```

ScratchBird sessions are always in a transaction. `commit()` / `rollback()` drain the
immediate reopen boundary before returning so the next statement sees real result frames.
`beginTransaction()` and `beginTransactionEx()` restart the current boundary.

Isolation aliases: `READ COMMITTED` => canonical `READ COMMITTED`;
`REPEATABLE READ` => canonical `SNAPSHOT`; `SERIALIZABLE` => canonical
`SNAPSHOT TABLE STABILITY`.

`Protocol::canonicalReadCommittedModeLabel($mode)` returns the canonical label for
auditing.

Retry scope:
```php
use ScratchBird\ErrorMapper;
$scope = ErrorMapper::retryScopeForSqlState("40001"); // "statement"|"reconnect"|"none"
```

---

## Type mapping

The driver maps to the `sbsql_core` profile via `ScratchBird\TypeDecoder`. PHP
types received from the wire:

| SBSQL canonical type | PHP type |
|---|---|
| BOOLEAN | `bool` |
| SMALLINT / INTEGER / BIGINT | `int` |
| REAL / DOUBLE PRECISION | `float` |
| NUMERIC / DECIMAL | `string` (preserves precision) |
| TEXT / VARCHAR | `string` |
| BYTEA | `string` (binary) |
| DATE | `string` (ISO 8601) |
| TIME | `string` (ISO 8601) |
| TIMESTAMP | `string` (ISO 8601) |
| TIMESTAMPTZ | `string` (ISO 8601 with timezone) |
| INTERVAL | `string` |
| UUID | `string` |
| JSON | `array` / `string` (parsed) |
| JSONB | `ScratchBird\Jsonb` (object) |
| ARRAY | `array` |
| RANGE | `ScratchBird\Range` (object) |
| Geometry | `ScratchBird\Geometry` (object) |

See [../type_mapping.md](../type_mapping.md) for the full type reference.

---

## Metadata and introspection

```php
// Named collection
$rows = $conn->queryMetadata("tables");

// With restrictions
$rows = $conn->getSchema("columns", ["schema" => "public", "table" => "items"]);

// Recursive schema tree
$tree = $conn->getSchemaTree("my_schema", expandParents: true);
```

Supported collections: `schemas`, `tables`, `columns`, `indexes`, `index_columns`,
`constraints`, `primary_keys`, `foreign_keys`, `table_privileges`, `column_privileges`,
`procedures`, `functions`, `type_info`, `catalogs`, `routines`.

`ScratchBird\Metadata` provides `buildMetadataSchemaTree`, `expandSchemaMetadataRows`,
`normalizeRestrictions`, and `filterRowsByRestrictions` for tree shaping and
restriction-aware filtering.

PDO wrappers: `$pdo->getSchema(collection)` and `$pdo->getSchemaTree(pattern)` are
also available.

See [../metadata_sys_information.md](../metadata_sys_information.md).

---

## Errors and diagnostics

Errors are thrown as typed subclasses of `ScratchBird\Errors\ScratchBirdException`:

```php
use ScratchBird\Errors\{
    ScratchBirdConnectionException,
    ScratchBirdTransactionException,
    ScratchBirdNotSupportedException,
};

try {
    $conn->query("bad sql");
} catch (ScratchBirdTransactionException $e) {
    $scope = ErrorMapper::retryScopeForSqlState($e->getSqlState());
} catch (ScratchBirdConnectionException $e) {
    // reconnect
}
```

`Connection::resumePortal()` fails closed with SQLSTATE `55000` unless the server first
reported `MSG_PORTAL_SUSPENDED`. `Connection::supportsDormantReattach()` returns
`false`; dormant methods fail closed with `0A000`.

See [../diagnostics_and_sqlstate.md](../diagnostics_and_sqlstate.md).

---

## Pooling and concurrency

The driver is rated `connection_thread_confined` — each `Connection` or `ScratchBirdPDO`
instance must be used from a single thread/request. Pooling at the PHP FPM / application
level (e.g., persistent connections or a pool library) is appropriate. The driver's
`session_pool` capability means individual session lifecycle helpers are present in the
driver.

See [../pooling_and_concurrency.md](../pooling_and_concurrency.md).

---

## Conformance

This driver targets conformance profile `driver_php_gate`. All seven JDBCBL capability
groups (CONN, TXN, EXEC, META, TYPE, ERR, RES) are status `Implemented`.

See [../conformance_baseline.md](../conformance_baseline.md).

---

## See also

- [../README.md](../README.md) — Client and Driver Guide overview
- [../connection_and_dsn.md](../connection_and_dsn.md) — DSN reference
- [../authentication.md](../authentication.md) — Auth methods
- [../wire_protocol_sbwp.md](../wire_protocol_sbwp.md) — SBWP v1.1
