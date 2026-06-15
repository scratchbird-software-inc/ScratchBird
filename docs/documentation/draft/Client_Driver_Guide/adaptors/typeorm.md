# TypeORM Adaptor

The TypeORM adaptor (`@scratchbird/typeorm-adapter`) provides adapter-level contracts for using ScratchBird with TypeORM. It normalizes TypeORM `DataSource` options, maps ScratchBird types to TypeORM column types, generates TypeORM-style entity schemas from ScratchBird metadata catalogs, and builds transaction command plans for nested CRUD operations. The adaptor delegates all wire transport to the ScratchBird Node.js driver.

**Status:** beta_2 (release_candidate bucket). The source README explicitly states: "Full live TypeORM runtime execution against managed/listener endpoints remains environment-gated." The package provides deterministic adapter contracts; a full live TypeORM `DataSource` session against a running ScratchBird server is not yet covered in the contract suite.

**Conformance profile:** `adaptor_typeorm_gate`

---

## Manifest metadata

| Field | Value |
|---|---|
| component\_id | `adaptor:scratchbird-typeorm-adapter` |
| driver\_package\_uuid | `019e12a0-0023-7000-8000-000000000023` |
| api\_surface | `application_adapter` |
| ingress\_mode | `driver_embedded_node` |
| delegates to / pooling | `delegates_to_node` (driver:node) |
| type\_mapping\_profile | `sbsql_core` |
| DSN / connection keys | `database`, `host`, `port`, `user`, `auth_method` |
| auth\_method\_set | `engine_local_password`, `scram_ready` |
| tls\_profile | `scratchbird_tls_1_3_floor` |
| thread\_safety\_class | `connection_thread_confined` |
| diagnostic\_mapping | `native_sqlstate` |
| metadata\_profile | `sys_information_recursive` |
| release\_bucket | `release_candidate` |
| license | MPL-2.0 |

---

## Installation

The adaptor is distributed as an npm package. Node.js 18 or later is required.

**Package name** (from `package.json`):

```
@scratchbird/typeorm-adapter
```

**Install:**

```bash
npm install @scratchbird/typeorm-adapter
```

The ScratchBird Node.js driver must also be available. The adaptor `main` entry point is `lib/index.js`.

---

## Configuring a connection

TypeORM is configured via a `DataSource` options object. The adaptor's `normalizeTypeOrmOptions` function (in `lib/options.js`) accepts either a URL string or individual host/port/database fields, merges any `extra` query parameters, and applies policy guardrails.

**Options object** (from `examples/sample-service.js`):

```javascript
const adapter = require("@scratchbird/typeorm-adapter");

const options = adapter.normalizeTypeOrmOptions({
  url: "scratchbird://sb_admin:change_me@127.0.0.1:3092/main?sslmode=require&binaryTransfer=true",
  extra: {
    connectTimeout: "30",
  },
});
// options.type === "scratchbird"
// options.host === "127.0.0.1"
// options.port === 3092
// options.database === "main"
```

**URL structure:**

```
scratchbird://<user>:<password>@<host>:<port>/<database>[?param=value&...]
```

Default host is `localhost`, default port is `3092`, default database is `main`. The URL protocol must be `scratchbird:`; any other protocol is rejected.

**Extra option guardrails** (`enforceGuardrails` in `lib/options.js`):

The adaptor normalizes connection option aliases and enforces these policy rules before any driver call:

| Parameter | Rejected value | Error |
|---|---|---|
| `sslmode` (or `ssl`) | `disable` | sslmode=disable is not supported |
| `binary_transfer` (or `binaryTransfer`) | `false` | binaryTransfer=false is not supported |
| `compression` | `zstd` | compression=zstd is not supported |
| `front_door_mode` | other than `direct` / `manager_proxy` | must be direct or manager\_proxy |
| `auth_method_id` | not prefixed `scratchbird.auth.` | must start with scratchbird.auth. |

---

## Type and feature mapping

`lib/type-map.js` provides `mapScratchBirdTypeToTypeOrm`. Representative entries from source:

| ScratchBird type | TypeORM column type |
|---|---|
| BOOLEAN | boolean |
| SMALLINT | smallint |
| INTEGER / INT | int |
| BIGINT | bigint |
| REAL / FLOAT | float |
| DOUBLE PRECISION | double precision |
| NUMERIC | numeric |
| DECIMAL | decimal |
| CHAR | char |
| VARCHAR / CHARACTER VARYING | varchar |
| TEXT | text |
| DATE | date |
| TIME | time |
| TIMESTAMP | timestamp |
| TIMESTAMPTZ | timestamptz |
| UUID | uuid |
| JSON | json |
| JSONB | jsonb |
| BYTEA / BLOB | bytea |
| VECTOR | text (unsupported flag) |
| GEOMETRY | bytea (unsupported flag) |

Types not in the map default to `varchar` with `unsupported: true`. Array-suffix types (e.g. `TEXT[]`) are flagged as array variants.

For the full type mapping reference see [../type_mapping.md](../type_mapping.md).

---

## Capabilities and limitations

**Supported (confirmed in source):**

- `normalizeTypeOrmOptions` — merges URL and explicit options, applies guardrails
- ScratchBird-to-TypeORM column type mapping (`mapScratchBirdTypeToTypeOrm`)
- Metadata-to-TypeORM entity schema generation (`lib/entity-schema.js` / `generateEntitySchemas`)
- Relation mapping (many-to-one, join column resolution) via entity schema generator
- Transaction command plan builder for nested CRUD (`buildNestedCrudTransactionPlan`) — plans request engine-side finality without taking finality locally
- Full canonical option alias normalization (manager-proxy, token/assertion, channel-binding, dormant-reattach)

**Not yet available / environment-gated (per source README):**

- Full live TypeORM `DataSource` session against a running ScratchBird server
- TypeORM migration (`synchronize`, `runMigrations`) — transaction plan builder provides contract scaffolding only
- Repository/EntityManager query lifecycle verification

All connection-level authority is delegated to the ScratchBird Node.js driver and revalidated server-side. The adaptor has `driver_local_authority: advisory_only`. Transaction finality is engine-owned (MGA); the adapter plans transaction commands but does not own commit/rollback.

---

## Diagnostics

SQLSTATE codes follow the `native_sqlstate` profile. See [../diagnostics_and_sqlstate.md](../diagnostics_and_sqlstate.md).

---

## See also

- [../README.md](../README.md) — Client and Driver Guide overview
- [../drivers/node.md](../drivers/node.md) — ScratchBird Node.js driver (the underlying transport)
- [../connection_and_dsn.md](../connection_and_dsn.md) — DSN and connection parameter reference
- [../authentication.md](../authentication.md) — Authentication methods
- [../type_mapping.md](../type_mapping.md) — Full type mapping reference
- [../diagnostics_and_sqlstate.md](../diagnostics_and_sqlstate.md) — SQLSTATE and error mapping
