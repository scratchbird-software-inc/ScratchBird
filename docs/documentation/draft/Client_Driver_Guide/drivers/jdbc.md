# ScratchBird JDBC Driver — JDBC 4.x

The ScratchBird JDBC driver is a pure-Java Type 4 driver that connects Java
applications to a ScratchBird Convergent Data Engine (CDE) over the ScratchBird
Native Wire Protocol (SBWP v1.1). It requires no native shared library and
implements the standard `java.sql` / `javax.sql` interfaces defined by JDBC 4.x.

**Release status: beta_2 (release_candidate gate)**

---

## Manifest metadata

| Field | Value |
|---|---|
| `driver_package_uuid` | `019e12a0-0006-7000-8000-000000000006` |
| `api_surface_set` | `jdbc_4_x` |
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
| `conformance_profile_ref` | `driver_jdbc_gate` |

---

## Installation and packaging

The driver is published as a Maven artifact:

```
groupId:    com.scratchbird
artifactId: scratchbird-jdbc
version:    0.1.0
```

Build from source with Gradle (requires JDK 17):

```bash
./gradlew build        # Linux / macOS
gradlew.bat build      # Windows
```

The resulting JAR includes `Automatic-Module-Name: com.scratchbird.jdbc`.
Minimum Java version: 17.

Integration test environment variables:
- `SCRATCHBIRD_JDBC_URL`
- `SCRATCHBIRD_JDBC_USER`
- `SCRATCHBIRD_JDBC_PASSWORD`

---

## Connecting

### JDBC URL form

```
jdbc:scratchbird://host:3092/database
```

Query parameters accepted on the URL: `sslmode`, `user`, `password`,
`auth_token`, `front_door_mode`, `manager_auth_token`, and startup plugin
selection fields.

### Loading the driver

```java
import com.scratchbird.jdbc.SBDriver;

// Auto-registration via ServiceLoader (Java 9+):
Connection conn = DriverManager.getConnection(
    "jdbc:scratchbird://db.example.com:3092/prod",
    "alice", "secret"
);

// Or construct directly:
SBDriver driver = new SBDriver();
Connection conn = driver.connect(
    "jdbc:scratchbird://db.example.com:3092/prod", props
);
```

### Connection properties (`SBConnectionProperties`)

Key properties (passed via `java.util.Properties` or a
`SBConnectionProperties` object):

| Key | Description |
|---|---|
| `user` | Database user |
| `password` | Password for `PASSWORD`/`SCRAM` auth |
| `auth_token` | Bearer token for `TOKEN` auth |
| `sslmode` | `disable`, `require`, `verify-ca`, `verify-full` |
| `front_door_mode` | `direct` (default) or `manager_proxy` |
| `manager_auth_token` | Required when `front_door_mode=manager_proxy` |

### Auth preflight probe

```java
SBAuthProbeResult probe = SBDriver.probeAuthSurface(url, props);
// or on an existing connection:
SBAuthProbeResult probe = conn.probeAuthSurface(props);
SBResolvedAuthContext ctx = conn.getResolvedAuthContext();
```

Directly executable auth classes: `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`,
`TOKEN`. `MD5`, `PEER`, and `REATTACH` are fail-closed (`0A000`).

The default port is `3092`; the default `sslmode` is `require`.

---

## Executing statements and transactions

### Statement execution

```java
try (Statement stmt = conn.createStatement()) {
    ResultSet rs = stmt.executeQuery("SELECT id, name FROM users WHERE id = 1");
    while (rs.next()) {
        System.out.println(rs.getLong("id") + " " + rs.getString("name"));
    }
}
```

### Prepared statements with parameter binding

```java
try (PreparedStatement ps = conn.prepareStatement(
        "INSERT INTO events (ts, kind) VALUES (?, ?)")) {
    ps.setTimestamp(1, ts);
    ps.setString(2, "click");
    ps.executeUpdate();
    System.out.println(ps.getLargeUpdateCount());
}
```

### Callable statements (stored procedures)

```java
try (CallableStatement cs = conn.prepareCall("{ call my_proc(?, ?) }")) {
    cs.setInt(1, 42);
    cs.setString(2, "arg");
    cs.execute();
}
```

### Multiple result sets

```java
stmt.execute("SELECT 1; SELECT 2");
do {
    ResultSet rs = stmt.getResultSet();
    // consume rs
} while (stmt.getMoreResults());
```

### Generated keys

```java
stmt.executeUpdate(
    "INSERT INTO orders (amount) VALUES (99.5)",
    Statement.RETURN_GENERATED_KEYS
);
ResultSet keys = stmt.getGeneratedKeys();
```

### Transaction control

ScratchBird sessions are always in a transaction. `COMMIT` / `ROLLBACK`
immediately reopen the next boundary.

```java
conn.setAutoCommit(false);
conn.commit();
conn.rollback();

Savepoint sp = conn.setSavepoint("sp1");
conn.rollback(sp);
conn.releaseSavepoint(sp);
```

### Isolation levels and canonical MGA mapping

The `setTransactionIsolation(int)` mapping:

| JDBC constant | Canonical MGA mode |
|---|---|
| `TRANSACTION_READ_UNCOMMITTED` | Legacy alias only |
| `TRANSACTION_READ_COMMITTED` | `READ COMMITTED` |
| `TRANSACTION_REPEATABLE_READ` / `SNAPSHOT` | `SNAPSHOT` |
| `TRANSACTION_SERIALIZABLE` | `SNAPSHOT TABLE STABILITY` |

The `READ COMMITTED` sub-mode selector:

```java
conn.setReadCommittedMode(SBConnection.READ_COMMITTED_MODE_READ_CONSISTENCY);
// or using the two-arg form:
conn.setTransactionIsolation(Connection.TRANSACTION_READ_COMMITTED,
    SBConnection.READ_COMMITTED_MODE_READ_CONSISTENCY);
```

`canonicalReadCommittedModeLabel(mode)` returns the canonical MGA label
string for audit and test purposes.

### Prepared / limbo transaction control

```java
conn.prepareTransaction("xid1");
conn.commitPrepared("xid1");
conn.rollbackPrepared("xid1");
```

`supportsDormantReattach()` returns `false` on this driver; `detachToDormant()`
and `reattachDormant(...)` fail closed with `0A000`.

### Retry scope

`SBProtocolHandler.retryScopeForSqlState(sqlstate)` returns
`SBRetryScope.STATEMENT` for `40001`/`40P01`,
`SBRetryScope.RECONNECT` for `08xxx`, and
`SBRetryScope.NONE` otherwise.

---

## Type mapping

The `sbsql_core` profile applies. Key JDBC type mappings:

| SBsql canonical type | Java type (`getObject`) | JDBC type constant |
|---|---|---|
| `BOOLEAN` | `Boolean` | `Types.BOOLEAN` |
| `SMALLINT` | `Short` | `Types.SMALLINT` |
| `INTEGER` | `Integer` | `Types.INTEGER` |
| `BIGINT` | `Long` | `Types.BIGINT` |
| `REAL` | `Float` | `Types.REAL` |
| `DOUBLE PRECISION` | `Double` | `Types.DOUBLE` |
| `NUMERIC` / `DECIMAL` | `java.math.BigDecimal` | `Types.NUMERIC` |
| `TEXT` / `VARCHAR` | `String` | `Types.VARCHAR` |
| `BYTEA` | `byte[]` | `Types.BINARY` |
| `DATE` | `java.sql.Date` | `Types.DATE` |
| `TIME` | `java.sql.Time` | `Types.TIME` |
| `TIMETZ` | `java.sql.Time` (offset-aware) | `Types.TIME_WITH_TIMEZONE` |
| `TIMESTAMP` | `java.sql.Timestamp` | `Types.TIMESTAMP` |
| `TIMESTAMPTZ` | `java.sql.Timestamp` (UTC) | `Types.TIMESTAMP_WITH_TIMEZONE` |
| `UUID` | `java.util.UUID` | `Types.OTHER` |
| `JSON` / `JSONB` | `SBJsonb` | `Types.OTHER` |
| `XML` | `SBSQLXML` | `Types.SQLXML` |
| array types | `SBArray` | `Types.ARRAY` |
| range types | `SBRange` | `Types.OTHER` |
| geometry | `SBGeometry` | `Types.OTHER` |
| `BLOB` | `SBBlob` | `Types.BLOB` |
| `CLOB` | `SBClob` | `Types.CLOB` |
| `REF` | `SBRef` | `Types.REF` |
| `ROWID` | `SBRowId` | `Types.ROWID` |

Type encoding and decoding is handled by `SBTypeCodec`. Extended type retrieval
uses `ResultSet.getObject(col, Class<T>)`.

See [../type_mapping.md](../type_mapping.md).

---

## Metadata and introspection

`DatabaseMetaData` is exposed through `SBDatabaseMetaData`:

```java
DatabaseMetaData meta = conn.getMetaData();
ResultSet tables = meta.getTables(null, "public", "%", new String[]{"TABLE"});
ResultSet cols   = meta.getColumns(null, "public", "orders", "%");
ResultSet pks    = meta.getPrimaryKeys(null, "public", "orders");
```

`ResultSetMetaData` is returned by `SBResultSetMetaData`.
All metadata queries target `sys.information.*` views on the engine.

See [../metadata_sys_information.md](../metadata_sys_information.md).

---

## Errors and diagnostics

Server errors arrive as `java.sql.SQLException` subclasses. SQLSTATE codes are
preserved in `SQLException.getSQLState()`. The retry-scope helper
`SBProtocolHandler.retryScopeForSqlState(sqlstate)` classifies errors for
retry decisions. Connection failures use `08xxx` SQLSTATE codes;
constraint violations use `23xxx`.

See [../diagnostics_and_sqlstate.md](../diagnostics_and_sqlstate.md).

---

## Pooling and concurrency

The driver is `thread_safe`. `SBConnectionPool` provides connection pooling with
lease/borrow lifecycle, stale-connection eviction, and keepalive support. For
applications using a third-party pool (HikariCP, c3p0, etc.), the driver
implements `javax.sql.DataSource` through `SBConnectionPool`. Pool-reset logic
rolls back abandoned transaction state on checkout.

See [../pooling_and_concurrency.md](../pooling_and_concurrency.md).

---

## Conformance

Conformance gate: `driver_jdbc_gate`. All JDBCBL groups (CONN, TXN, EXEC,
META, TYPE, ERR, RES) are implemented per `BASELINE_REQUIREMENT_MAPPING.md`.

See [../conformance_baseline.md](../conformance_baseline.md).

---

## See also

- [../README.md](../README.md)
- [../connection_and_dsn.md](../connection_and_dsn.md)
- [../authentication.md](../authentication.md)
- [../wire_protocol_sbwp.md](../wire_protocol_sbwp.md)
