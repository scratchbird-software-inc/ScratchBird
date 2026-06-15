# Node.js / TypeScript Driver — Async language binding for ScratchBird

The Node.js driver (`scratchbird` npm package) is a native TypeScript client for
ScratchBird. It speaks SBWP v1.1 over TCP/TLS, exports a `Client` class with
`async/await` methods and full TypeScript type declarations, and includes a `Pool` class
for connection pooling. It is the idiomatic choice for Node.js / TypeScript applications.

Release status: **beta_2** (release_candidate gate). CI coverage on Linux and Windows.
macOS is not currently in CI.

---

## Driver metadata

| Field | Value |
|---|---|
| Driver package UUID | `019e12a0-0008-7000-8000-000000000008` |
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
| Conformance profile ref | `driver_node_gate` |

---

## Installation

```bash
npm install scratchbird
```

Package name: `scratchbird`, version 0.1.0, license MPL-2.0. Main entry point:
`dist/index.js`. TypeScript declarations: `dist/index.d.ts`.

Build from source:
```bash
npm install
npm run build   # tsc -p tsconfig.json
```

---

## Connecting

### Object config (recommended)

```typescript
import { Client } from "scratchbird";

const client = new Client({
  host: "localhost",
  port: 3092,
  user: "user",
  password: "pass",
  database: "db",
});

await client.connect();
const res = await client.query("SELECT 1 AS one");
console.log(res.rows);
await client.end();
```

### DSN string

```typescript
const client = new Client({
  connectionString: "scratchbird://user:pass@localhost:3092/mydb",
});
```

### TLS

```typescript
const client = new Client({
  host: "localhost",
  user: "user",
  password: "pass",
  database: "db",
  sslmode: "verify-full",
  sslrootcert: "/etc/ssl/certs/ca.pem",
});
```

### Key `ClientConfig` fields

| Field | Description |
|---|---|
| `host` | Server hostname |
| `port` | Default 3092 |
| `database` | Target database |
| `user` | Login user |
| `password` | Password |
| `authMethod` | `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`, `TOKEN` |
| `sslmode` | `disable`, `require`, `verify-ca`, `verify-full` |
| `frontDoorMode` | `direct_listener` or `manager_proxy` |
| `managerAuthToken` | Required when `frontDoorMode=manager_proxy` |
| `authToken` | Bearer token for TOKEN auth |
| `metadataExpandSchemaParents` | Enable recursive parent expansion (also: `metadata_expand_schema_parents`, `expandSchemaParents`) |

### Auth probe (staged bootstrap)

```typescript
import { Client } from "scratchbird";

const probe = await Client.probeAuthSurface({ host: "localhost", port: 3092 });
// or on a connected client: client.probeAuthSurface()
const ctx = client.getResolvedAuthContext();
```

Admitted-but-unsupported methods (`MD5`, `PEER`) fail closed. Integration test DSN:
set `SCRATCHBIRD_NODE_URL` or `SCRATCHBIRD_TEST_DSN`.

---

## Executing statements and transactions

### Query and execute

```typescript
// Simple query
const res = await client.query("SELECT id, name FROM items");
console.log(res.rows); // Array<Record<string, any>>

// Parameterized (positional)
const res2 = await client.query(
  "SELECT id FROM items WHERE id = $1", [42]
);

// Execute (no rows)
await client.query("DELETE FROM items WHERE id = $1", [99]);
```

### Multi-result, batch, callable, streaming

```typescript
// Multi-result
const results = await client.queryMulti("SELECT 1; SELECT 2");

// Batch
const summary = await client.executeBatch(
  "INSERT INTO t VALUES ($1)", [[1], [2], [3]]
);

// Generated keys
const keys = await client.executeWithGeneratedKeys(
  "INSERT INTO t(name) VALUES ($1) RETURNING id", ["Alice"]
);

// Callable (JDBC escape syntax)
const callResult = await client.call("{ call my_proc(?) }", [arg]);

// Streaming with pagination
const stream = await client.queryStream("SELECT * FROM big_table", [], { maxRows: 100 });
for await (const batch of stream) { /* process batch */ }
```

### Native SQL normalization

```typescript
const normalized = client.nativeSQL("SELECT * FROM t WHERE id = ?");
const callable = client.nativeCallableSQL("{ call proc(?) }");
```

### Session schema

```typescript
await client.setSessionSchema("my_schema");
const current = client.getSessionSchema();
```

### Transaction control

```typescript
// Begin with options
await client.beginTransaction({
  isolationLevel: "READ COMMITTED",
  readCommittedMode: "READ COMMITTED READ CONSISTENCY",
  accessMode: "READ WRITE",
});

await client.savepoint("sp1");
await client.rollbackToSavepoint("sp1");
await client.releaseSavepoint("sp1");
await client.commitTransaction();
// or:
await client.rollbackTransaction();
```

ScratchBird sessions are always in a transaction. `commitTransaction()` /
`rollbackTransaction()` immediately reopen the next boundary.

Isolation aliases: `READ COMMITTED` => canonical `READ COMMITTED`;
`REPEATABLE READ` => canonical `SNAPSHOT`; `SERIALIZABLE` => canonical
`SNAPSHOT TABLE STABILITY`.

Autocommit:
```typescript
await client.setAutoCommit(true);
await client.setAutoCommit(false); // eager begin
const ac = client.getAutoCommit();
```

Retry scope:
```typescript
import { retryScopeForSqlState } from "scratchbird";
const scope = retryScopeForSqlState("40001"); // "statement"
```

---

## Type mapping

Typed value classes are exported from the package for encoding complex types.

| SBSQL canonical type | TypeScript / JavaScript type |
|---|---|
| BOOLEAN | `boolean` |
| SMALLINT / INTEGER / BIGINT | `number` / `bigint` |
| REAL / DOUBLE PRECISION | `number` |
| NUMERIC / DECIMAL | `ScratchbirdDecimal` |
| TEXT / VARCHAR | `string` |
| BYTEA | `Buffer` |
| DATE | `ScratchbirdDate` |
| TIME | `ScratchbirdTime` |
| TIMESTAMP | `ScratchbirdTimestamp` |
| TIMESTAMPTZ | `ScratchbirdTimestampTZ` |
| INTERVAL | `ScratchbirdInterval` |
| UUID | `string` |
| JSON | `ScratchbirdJson` |
| JSONB | `ScratchbirdJsonb` |
| MONEY | `ScratchbirdMoney` |
| ARRAY | `any[]` |
| RANGE | `ScratchbirdRange<T>` |
| Geometry | `ScratchbirdGeometry` |

See [../type_mapping.md](../type_mapping.md) for the full type reference.

---

## Metadata and introspection

```typescript
// Named collection
const schemas = await client.queryMetadata("schemas");
const tables  = await client.getSchema("tables", { schema: "public" });

// Recursive schema tree
const tree = await client.getSchema("schemas"); // with metadataExpandSchemaParents config
```

Supported collections: `schemas`, `tables`, `columns`, `indexes`, `index_columns`,
`constraints`, `primary_keys`, `foreign_keys`, `table_privileges`, `column_privileges`,
`procedures`, `functions`, `type_info`, `catalogs`.

See [../metadata_sys_information.md](../metadata_sys_information.md).

---

## Errors and diagnostics

Errors are thrown as typed classes:

```typescript
import { ScratchbirdConnectionError, ScratchbirdTransactionError } from "scratchbird";

try {
  await client.query("bad sql");
} catch (e) {
  if (e instanceof ScratchbirdTransactionError) {
    const scope = retryScopeForSqlState(e.sqlState);
    // scope: "statement" | "reconnect" | "none"
  }
}
```

Normalization failures map to `ScratchbirdSyntaxError` (SQLSTATE `07001`).

See [../diagnostics_and_sqlstate.md](../diagnostics_and_sqlstate.md).

---

## Pooling and concurrency

```typescript
import { Pool } from "scratchbird";

const pool = new Pool({
  host: "localhost", port: 3092, user: "user", password: "pass", database: "db",
  max: 10,
});
const client = await pool.connect();
await client.query("SELECT 1");
client.release();
await pool.end();
```

See [../pooling_and_concurrency.md](../pooling_and_concurrency.md).

---

## Conformance

This driver targets conformance profile `driver_node_gate`. All seven JDBCBL capability
groups (CONN, TXN, EXEC, META, TYPE, ERR, RES) are status `Implemented`.

See [../conformance_baseline.md](../conformance_baseline.md).

---

## See also

- [../README.md](../README.md) — Client and Driver Guide overview
- [../connection_and_dsn.md](../connection_and_dsn.md) — DSN reference
- [../authentication.md](../authentication.md) — Auth methods
- [../wire_protocol_sbwp.md](../wire_protocol_sbwp.md) — SBWP v1.1
