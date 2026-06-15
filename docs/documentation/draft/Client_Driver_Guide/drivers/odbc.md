# ScratchBird ODBC Driver — ODBC 3.x (3.8)

The ScratchBird ODBC driver is a shared-library ODBC 3.8 driver (Windows: DLL,
Linux: `.so`) that connects ODBC-capable applications to a ScratchBird
Convergent Data Engine (CDE) over the ScratchBird Native Wire Protocol (SBWP
v1.1). Applications link against the standard ODBC Driver Manager; no
ScratchBird-specific library is loaded by application code.

**Release status: beta_2 (release_candidate gate)**

---

## Manifest metadata

| Field | Value |
|---|---|
| `driver_package_uuid` | `019e12a0-0009-7000-8000-000000000009` |
| `api_surface_set` | `odbc_3_x` |
| `ingress_mode_set` | `direct_listener`, `manager_proxy` |
| `wire_protocol_set` | `sbwp_v1_1` |
| `dsn_key_set` | `database`, `host`, `port`, `dsn`, `user`, `auth_method` |
| `auth_method_set` | `engine_local_password`, `scram_ready` |
| `tls_profile_set` | `scratchbird_tls_1_3_floor` |
| `type_mapping_profile` | `sbsql_core` |
| `diagnostic_mapping_profile` | `native_sqlstate` |
| `metadata_profile` | `sys_information_recursive` |
| `thread_safety_class` | `thread_safe` |
| `pooling_capability` | `connection_pool` |
| `release_bucket` | `release_candidate` |
| `conformance_profile_ref` | `driver_odbc_gate` |

---

## Building the driver

The driver is built with CMake (C++17, C99). Platform targets: Linux and
Windows (CI-covered); macOS is untested.

```bash
cmake -S . -B build
cmake --build build --config Release
```

The CMake project name is `scratchbird_odbc`, version `0.1.0`. The shared
library output is `scratchbird_odbc` (`.so` on Linux, `.dll` on Windows). See
`docs/BUILD_MATRIX.md` for required ODBC Manager and OpenSSL dependencies.

---

## Connecting

### Connection string (direct/native)

```ini
Driver={ScratchBird};Server=127.0.0.1;Port=3092;Database=mydb;UID=user;PWD=pass;SSLMode=prefer
```

### Connection string (token auth)

```ini
Driver={ScratchBird};Server=127.0.0.1;Port=3092;Database=mydb;UID=user;AuthMethodId=scratchbird.auth.token;AuthToken=token
```

`AuthToken`, `BearerToken`, and `Token` are accepted aliases and normalized to
the bridge config.

### Connection string (manager-proxy ingress)

```ini
Driver={ScratchBird};Server=127.0.0.1;Port=3090;Database=mydb;UID=user;PWD=pass;FrontDoorMode=manager_proxy;ManagerAuthToken=token
```

### DSN entry (odbc.ini / registry)

The driver registers under the name `ScratchBird`. `DSN=<name>` is an accepted
DSN key in the connection string.

### Key reference

| Key | Description |
|---|---|
| `Server` / `Host` | Engine hostname or IP |
| `Port` | Engine port (default `3092`) |
| `Database` | Target database name |
| `UID` / `User` | Login user |
| `PWD` / `Password` | Login password |
| `SSLMode` | `disable`, `prefer`, `require`, `verify-ca`, `verify-full` |
| `AuthMethodId` | Auth plugin identifier |
| `AuthToken` / `BearerToken` / `Token` | Bearer token value |
| `FrontDoorMode` | `direct` (default) or `manager_proxy` |
| `ManagerAuthToken` | Proxy bearer token |

### Auth preflight probe

```c
// C-level via OdbcClientBridge:
OdbcClientBridge::probeAuthSurface(connectionString, outResult);
OdbcClientBridge::getResolvedAuthContext();
```

Directly executable auth classes: `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`,
`TOKEN`. `MD5`, `PEER`, and `REATTACH` are fail-closed.

---

## Executing statements and transactions

### Basic execution (SQLExecDirect / SQLExecute)

```c
SQLHSTMT hstmt;
SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt);
SQLExecDirect(hstmt,
    (SQLCHAR*)"SELECT id FROM users WHERE region = ?", SQL_NTS);
```

`SQL_SUCCESS_WITH_INFO` is treated as successful for both `SQLExecute` and
`SQLExecDirect` (not as a hard failure).

### Parameter binding (SQLBindParameter)

```c
SQLINTEGER region_len = SQL_NTS;
SQLCHAR region[32] = "west";
SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
    31, 0, region, sizeof(region), &region_len);
```

### Transaction control

```c
SQLSetConnectAttr(hdbc, SQL_ATTR_AUTOCOMMIT,
    (SQLPOINTER)SQL_AUTOCOMMIT_OFF, 0);

// commit or rollback at connection level:
SQLEndTran(SQL_HANDLE_DBC, hdbc, SQL_COMMIT);
SQLEndTran(SQL_HANDLE_DBC, hdbc, SQL_ROLLBACK);

// ENV-handle fan-out commits all connected child connections:
SQLEndTran(SQL_HANDLE_ENV, henv, SQL_COMMIT);
```

After `SQLEndTran(ROLLBACK)`, the next query is immediately usable on the
reopened MGA boundary with no reconnect.

### Isolation level mapping

Set via `SQL_ATTR_TXN_ISOLATION`:

| ODBC constant | Canonical MGA mode |
|---|---|
| `SQL_TXN_READ_UNCOMMITTED` | Legacy alias only |
| `SQL_TXN_READ_COMMITTED` | `READ COMMITTED` |
| `SQL_TXN_REPEATABLE_READ` / `VERSIONING` | `SNAPSHOT` |
| `SQL_TXN_SERIALIZABLE` | `SNAPSHOT TABLE STABILITY` |

A distinct `READ COMMITTED READ CONSISTENCY` selector is not yet exposed through
the `SQL_ATTR_TXN_ISOLATION` surface in this driver version.

### Prepared / dormant / portal capabilities

| Capability | Value |
|---|---|
| `supportsPreparedTransactions()` | present; builds canonical SQL |
| `supportsDormantReattach()` | false — fail-closed |
| `supportsPortalResume()` | false — intentionally absent |

Retry is SQLSTATE-driven: `40001`/`40P01` → fresh statement boundary;
`08xxx` → reconnect or reopen. The retry helper stops immediately on operator-
intervention diagnostics such as `57014`.

---

## Type mapping

The `sbsql_core` profile applies. Key ODBC C-type / SQL-type mappings:

| SBsql canonical type | ODBC SQL type | C type |
|---|---|---|
| `BOOLEAN` | `SQL_BIT` | `SQL_C_BIT` |
| `SMALLINT` | `SQL_SMALLINT` | `SQL_C_SHORT` |
| `INTEGER` | `SQL_INTEGER` | `SQL_C_LONG` |
| `BIGINT` | `SQL_BIGINT` | `SQL_C_SBIGINT` |
| `REAL` | `SQL_REAL` | `SQL_C_FLOAT` |
| `DOUBLE PRECISION` | `SQL_DOUBLE` | `SQL_C_DOUBLE` |
| `NUMERIC` / `DECIMAL` | `SQL_NUMERIC` | `SQL_C_NUMERIC` |
| `TEXT` / `VARCHAR` | `SQL_VARCHAR` | `SQL_C_CHAR` / `SQL_C_WCHAR` |
| `BYTEA` | `SQL_VARBINARY` | `SQL_C_BINARY` |
| `DATE` | `SQL_TYPE_DATE` | `SQL_C_TYPE_DATE` |
| `TIME` | `SQL_TYPE_TIME` | `SQL_C_TYPE_TIME` |
| `TIMETZ` | `SQL_TYPE_TIME` | `SQL_C_TYPE_TIME` |
| `TIMESTAMP` | `SQL_TYPE_TIMESTAMP` | `SQL_C_TYPE_TIMESTAMP` |
| `TIMESTAMPTZ` | `SQL_TYPE_TIMESTAMP` | `SQL_C_TYPE_TIMESTAMP` |
| `UUID` | `SQL_GUID` | `SQL_C_GUID` |

Type information is exposed through `SQLGetTypeInfo`. Unicode handling uses
`SQL_C_WCHAR` on Windows.

See [../type_mapping.md](../type_mapping.md).

---

## Metadata and introspection

Catalog functions expose `sys.information.*`:

```c
SQLTables(hstmt, NULL, 0, (SQLCHAR*)"public", SQL_NTS, NULL, 0, NULL, 0);
SQLColumns(hstmt, NULL, 0, (SQLCHAR*)"public", SQL_NTS,
    (SQLCHAR*)"orders", SQL_NTS, NULL, 0);
SQLPrimaryKeys(hstmt, NULL, 0, (SQLCHAR*)"public", SQL_NTS,
    (SQLCHAR*)"orders", SQL_NTS);
```

Recursive schema-tree navigation is supported through `SQLBrowseConnect`:
database → default branch rows → dotted parent expansion → leaf tables.
`SQLBrowseConnect` uses slash-delimited path splitting to preserve dotted
schema segments. Full executable metadata family parity is partial in this
release.

See [../metadata_sys_information.md](../metadata_sys_information.md).

---

## Errors and diagnostics

Diagnostics are retrieved with `SQLGetDiagRec`. SQLSTATE codes are native
ScratchBird codes (e.g., `40001` for serialization failure, `08001` for
connection failure). `SQLGetDiagField` with `SQL_DIAG_MESSAGE_TEXT` returns the
full engine message including DETAIL and HINT.

Disconnect clears statement handles, the prepared SQL cache, transaction flags,
and bridge session state before any later reconnect.

See [../diagnostics_and_sqlstate.md](../diagnostics_and_sqlstate.md).

---

## Pooling and concurrency

The driver is `thread_safe`. Connection pooling is managed through the ODBC
Driver Manager's standard connection pooling (`SQL_CP_ONE_PER_DRIVER` or
`SQL_CP_ONE_PER_HENV`). The driver's internal `statement_cache` module
(C++ class `StatementCache`) caches prepared statement handles at the
connection level. Circuit-breaker and keepalive support are present at the
bridge layer.

See [../pooling_and_concurrency.md](../pooling_and_concurrency.md).

---

## Conformance

Conformance gate: `driver_odbc_gate`. JDBCBL-equivalent groups: CONN, TXN,
EXEC, ERR, TYPE, RES are implemented; META is partial (recursive schema shaping
is present; full catalog family parity is incomplete).

See [../conformance_baseline.md](../conformance_baseline.md).

---

## See also

- [../README.md](../README.md)
- [../connection_and_dsn.md](../connection_and_dsn.md)
- [../authentication.md](../authentication.md)
- [../wire_protocol_sbwp.md](../wire_protocol_sbwp.md)
