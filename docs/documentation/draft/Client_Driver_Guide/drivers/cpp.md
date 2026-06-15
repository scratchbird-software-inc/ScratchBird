# C/C++ Driver — Native CLI C API (`libscratchbird_client`)

The C/C++ driver provides a dual-surface native client library for ScratchBird. The primary
surface is a stable C ABI (`scratchbird_client.h`) usable from any language with C FFI support.
A higher-level C++ surface is layered on top and exposes RAII wrappers, prepared statements,
connection pooling, and typed parameter binding. Both surfaces speak SBWP v1.1 (ScratchBird
Binary Wire Protocol) over TCP/TLS.

Release status: **beta_2** (release_candidate gate). CI coverage on Linux and Windows.
macOS is not currently in CI.

---

## Driver metadata

| Field | Value |
|---|---|
| Driver package UUID | `019e12a0-0001-7000-8000-000000000001` |
| API surface | `native_cli_c_api` |
| Ingress modes | `direct_listener`, `manager_proxy` |
| Wire protocol | `sbwp_v1_1` |
| DSN keys | `database`, `host`, `port`, `user`, `auth_method` |
| Auth methods | `engine_local_password`, `scram_ready` |
| TLS profile | `scratchbird_tls_1_3_floor` |
| Type mapping profile | `sbsql_core` |
| Diagnostic mapping profile | `native_sqlstate` |
| Metadata profile | `sys_information_recursive` |
| Thread safety | `thread_safe` |
| Pooling capability | `session_pool`, `statement_cache` |
| Conformance profile ref | `driver_cpp_gate` |

---

## Installation

The library is built with CMake. The project name and output library are `scratchbird_client`
(version 0.1.0, C++17, license MPL-2.0). OpenSSL is a required dependency.

```bash
cmake -S . -B build
cmake --build build --config Release
```

The public header to include is `<scratchbird/client/scratchbird_client.h>` (C API) or the
C++ headers under `include/scratchbird/client/` (`connection.h`, `network_client.h`,
`pool.h`, `metadata.h`).

---

## Connecting

### C API — DSN string

```c
#include <scratchbird/client/scratchbird_client.h>

sb_error err = {0};
sb_connection* conn =
    sb_connect("scratchbird://user:pass@127.0.0.1:3092/mydb", &err);
if (!conn) {
    /* err.code and err.message describe the failure */
    return 1;
}
/* use conn ... */
sb_disconnect(conn);
```

### DSN forms

URI:
```
scratchbird://user:password@host:3092/database?sslmode=require
```

Key-value:
```
host=localhost port=3092 dbname=mydb user=myuser password=mypass
```

### Key DSN parameters

| Key | Description |
|---|---|
| `host` | Server hostname or IP |
| `port` | Default 3092 |
| `dbname` / `database` | Target database |
| `user` | Login user |
| `password` | Password (PASSWORD or SCRAM auth) |
| `auth_method` | `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`, `TOKEN` |
| `sslmode` | `disable`, `require`, `verify-ca`, `verify-full` |
| `front_door_mode` | `direct_listener` (default) or `manager_proxy` |
| `manager_auth_token` | Required when `front_door_mode=manager_proxy` |
| `auth_token` | Bearer token for TOKEN auth |
| `compression` | `off` (default) or `zstd` |

### Auth probe (staged bootstrap)

Before a full connect, the driver can probe which auth methods the server advertises:

```c
char* json = sb_probe_auth_surface_json(
    "scratchbird://user@host:3092/mydb", &err);
/* json is driver-owned; release with sb_memory_release(json) */
```

The C++ equivalent is `scratchbird::client::probeAuthSurface(...)` or
`NetworkClient::probeAuthSurface(...)`.

After a successful connection, `sb_get_resolved_auth_context_json(conn, &err)` returns
the negotiated auth context.

Admitted-but-unsupported methods (`MD5`, `PEER`, `REATTACH`) fail closed with SQLSTATE
`0A000`.

---

## Executing statements and transactions

### C API: query and execute

```c
/* Simple query — returns result handle */
sb_result* result = sb_query(conn, "SELECT id, name FROM items", &err);
if (!result) { /* check err */ }

size_t nrows = sb_result_row_count(result);
size_t ncols = sb_result_col_count(result);
for (size_t r = 0; r < nrows; r++) {
    sb_row row = sb_result_get_row(result, r);
    sb_value val = sb_row_get_value_by_index(&row, 0);
    /* use val */
}
sb_result_free(result);

/* Execute (no rows returned) */
sb_execute(conn, "INSERT INTO items VALUES ($1)", &err);
```

### Prepared statements (C API)

```c
sb_prepared* stmt = sb_prepare(conn, "SELECT id FROM items WHERE id=$1", &err);
sb_bind_index(stmt, 0, SB_TYPE_INT4, &my_int, sizeof(my_int));
sb_result* res = sb_execute_prepared(stmt, &err);
sb_result_free(res);
sb_prepared_free(stmt);
```

The C++ surface exposes `PreparedStatement` with typed parameter setters:

```cpp
#include <scratchbird/client/connection.h>

scratchbird::client::Connection conn(...);
conn.open();
auto stmt = conn.prepare("SELECT id FROM items WHERE id = $1");
stmt.setInt(0, 42);
auto result = stmt.execute();
```

### Transaction control

```c
/* Begin */
sb_txn_options opts = {0};
opts.isolation = SB_TXN_READ_COMMITTED;      /* byte 1 */
/* opts.isolation = SB_TXN_SNAPSHOT;         byte 2 */
/* opts.isolation = SB_TXN_SNAPSHOT_TABLE_STABILITY; byte 3 */
sb_tx_begin(conn, &opts, &err);

/* Savepoint */
sb_tx_savepoint(conn, "sp1", &err);
sb_tx_rollback_to(conn, "sp1", &err);
sb_tx_release_savepoint(conn, "sp1", &err);

/* Commit / rollback */
sb_tx_commit(conn, &err);
sb_tx_rollback(conn, &err);
```

ScratchBird sessions are always in a transaction. `COMMIT` and `ROLLBACK` immediately
reopen the next transaction boundary. `sb_canonical_isolation_name(byte)` returns the
canonical isolation name for auditing.

Retry scope helper: `sb_retry_scope_for_sqlstate(sqlstate)` — returns
`40001`/`40P01` => fresh statement only; `08xxx` => reconnect only; everything else =>
no automatic replay.

The C++ `Connection` surface provides `beginTransaction(...)`, `commit()`, `rollback()`,
`savepoint()`, `releaseSavepoint()`, `rollbackToSavepoint()`, `prepareTransaction()`,
`commitPrepared()`, and `rollbackPrepared()`.

---

## Type mapping

The cpp driver maps to the `sbsql_core` type profile. The C API represents values via
`sb_value` / `sb_type` (an `sb_type_code` enum). The C++ surface uses `sb_value`
accessed through typed getters.

| SBSQL canonical type | C API sb_type_code | Notes |
|---|---|---|
| BOOLEAN | `SB_TYPE_BOOL` | |
| SMALLINT | `SB_TYPE_INT2` | |
| INTEGER | `SB_TYPE_INT4` | |
| BIGINT | `SB_TYPE_INT8` | |
| REAL | `SB_TYPE_FLOAT4` | |
| DOUBLE PRECISION | `SB_TYPE_FLOAT8` | |
| NUMERIC / DECIMAL | `SB_TYPE_NUMERIC` | |
| TEXT / VARCHAR / BPCHAR | `SB_TYPE_TEXT` / `SB_TYPE_VARCHAR` / `SB_TYPE_BPCHAR` | |
| BYTEA | `SB_TYPE_BYTEA` | |
| DATE | `SB_TYPE_DATE` | |
| TIME | `SB_TYPE_TIME` | |
| TIMESTAMP | `SB_TYPE_TIMESTAMP` | |
| TIMESTAMPTZ | `SB_TYPE_TIMESTAMPTZ` | |
| INTERVAL | `SB_TYPE_INTERVAL` | |
| UUID | `SB_TYPE_UUID` | |
| JSON | `SB_TYPE_JSON` | |
| JSONB | `SB_TYPE_JSONB` | |
| ARRAY | `SB_TYPE_ARRAY` | |
| RANGE | `SB_TYPE_RANGE` | |

See [../type_mapping.md](../type_mapping.md) for the full SBSQL canonical type set.

---

## Metadata and introspection

Metadata collections are accessed via `sb_metadata_query(conn, collection_name, &err)`,
which resolves the collection name and executes the appropriate `sys.*` query. Supported
collections include `schemas`, `tables`, `columns`, `indexes`, `index_columns`,
`constraints`, `primary_keys`, `foreign_keys`, `table_privileges`, `column_privileges`,
`procedures`, `functions`, `type_info`, `catalogs`, and `routines`.

Column metadata for a result is available via `sb_column_count(result)` and
`sb_get_column_meta(result, col_index, &meta_out)`.

The C++ surface provides `metadataSchemaPathsForNavigation(...)`,
`buildMetadataSchemaTree(...)`, `buildMetadataSchemaTreeRows(...)`, and
`buildMetadataDdlEditorSchemaPayloadJson(...)` in `metadata.h`.

See [../metadata_sys_information.md](../metadata_sys_information.md) for the
`sys.information.*` catalog family reference.

---

## Errors and diagnostics

The C API communicates errors via the caller-owned `sb_error` struct (an `sb_error_code`
plus a 256-byte message string). The driver never allocates `sb_error`.

```c
sb_error err = {0};
sb_result* r = sb_query(conn, "bad sql", &err);
if (!r) {
    printf("error %d: %s\n", err.code, err.message);
}
```

Key `sb_error_code` values: `SB_ERR_CONNECTION_FAILED`, `SB_ERR_AUTH_FAILED`,
`SB_ERR_TXN_CONFLICT`, `SB_ERR_DEADLOCK`, `SB_ERR_SYNTAX`, `SB_ERR_CONSTRAINT`,
`SB_ERR_TXN_ABORTED`, `SB_ERR_NOT_IMPLEMENTED`.

The SQLSTATE string is propagated through `ErrorContext` in the C++ layer. Use
`sb_retry_scope_for_sqlstate(sqlstate)` to classify whether a statement or a full
reconnect is warranted.

See [../diagnostics_and_sqlstate.md](../diagnostics_and_sqlstate.md) for SQLSTATE
classes and retry semantics.

---

## Memory ownership

Driver-allocated handles (`sb_connection`, `sb_result`, `sb_prepared`) must be released
with their matching `sb_*_free` function. `char*` values returned by the API are
driver-owned and must be released with `sb_memory_release(ptr)` or `sb_memory_free(ptr)`.
`const char*` and `sb_value` pointers borrowed from row/metadata callbacks are valid only
for the documented parent handle lifetime and must not be freed.

---

## Pooling and concurrency

The C++ surface provides `ConnectionPool` and `ConnectionLease` as RAII wrappers over the
pooled C API. The driver is rated `thread_safe`. A checked-out lease is exclusive to its
thread while held. A `LeakDetector` is built in for diagnosing unreturned leases.

See [../pooling_and_concurrency.md](../pooling_and_concurrency.md).

---

## Conformance

This driver targets conformance profile `driver_cpp_gate`. All seven JDBCBL capability
groups (CONN, TXN, EXEC, META, TYPE, ERR, RES) are status `Implemented` as of the current
S0 baseline. Remaining gaps are live integration depth, not missing API surfaces.

See [../conformance_baseline.md](../conformance_baseline.md).

---

## See also

- [../README.md](../README.md) — Client and Driver Guide overview
- [../connection_and_dsn.md](../connection_and_dsn.md) — DSN reference
- [../authentication.md](../authentication.md) — Auth methods
- [../wire_protocol_sbwp.md](../wire_protocol_sbwp.md) — SBWP v1.1
