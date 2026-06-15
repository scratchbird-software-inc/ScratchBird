# Prisma Adaptor

The Prisma adaptor (`scratchbird-prisma-adapter`) provides adapter-level scaffolding for using ScratchBird with Prisma. It handles datasource URL validation, ScratchBird-to-Prisma scalar/native type mapping, metadata-to-`schema.prisma` model generation, and migration/reflection workflow contracts. The adaptor delegates all wire transport to the ScratchBird Node.js driver.

**Status:** beta_2 (release_candidate bucket). This is not yet a full Prisma provider runtime. The source README explicitly states: "This is not yet a full Prisma provider runtime." The package provides adapter-level contracts and deterministic tests to de-risk datasource validation, scalar mapping, schema generation, and migration/reflection workflows. A live Prisma client query lifecycle is environment-gated.

**Conformance profile:** `adaptor_prisma_gate`

---

## Manifest metadata

| Field | Value |
|---|---|
| component\_id | `adaptor:scratchbird-prisma-adapter` |
| driver\_package\_uuid | `019e12a0-0020-7000-8000-000000000020` |
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
scratchbird-prisma-adapter
```

**Install:**

```bash
npm install scratchbird-prisma-adapter
```

The ScratchBird Node.js driver must also be available. The adaptor `main` entry point is `lib/index.js`.

---

## Configuring a connection

Connection URLs follow the `scratchbird://` scheme. From the source (`lib/connection-url.js`), the URL must use the `scratchbird:` protocol; any other scheme is rejected.

**URL structure:**

```
scratchbird://<user>:<password>@<host>:<port>/<database>[?param=value&...]
```

Default host is `localhost`, default port is `3092`.

**`schema.prisma` datasource block** (from `examples/schema.prisma`):

```prisma
datasource db {
  provider = "scratchbird"
  url      = env("DATABASE_URL")
}

generator client {
  provider = "prisma-client-js"
}
```

The adapter validates that the schema text contains a `datasource` block, a `generator` block, and that the datasource URL is set via `env("DATABASE_URL")`. Inline URL literals are not accepted by the schema validator.

**`DATABASE_URL` example:**

```
DATABASE_URL=scratchbird://sb_admin:change_me@127.0.0.1:3092/main?sslmode=require&binaryTransfer=true
```

**Connection parameter guardrails** (`normalizeScratchbirdQueryParams` in `lib/connection-url.js`):

The adaptor normalizes query parameter aliases and enforces the following policy before any driver call:

| Parameter | Rejected value | Error |
|---|---|---|
| `sslmode` (or `ssl`) | `disable` | sslmode=disable is not supported |
| `binary_transfer` | `false` | binary\_transfer=false is not supported |
| `compression` | `zstd` | compression=zstd is not supported |
| `front_door_mode` | other than `direct` / `manager_proxy` | must be direct or manager\_proxy |
| `auth_method_id` | not prefixed `scratchbird.auth.` | must start with scratchbird.auth. |

Manager-proxy, token/assertion, channel-binding, and dormant-reattach option families are all accepted as canonical query parameters.

---

## Type and feature mapping

`lib/type-map.js` provides `mapScratchBirdTypeToPrisma`. The mapping covers the Prisma scalar types. Representative entries:

| ScratchBird type | Prisma type | Native type annotation |
|---|---|---|
| BOOLEAN | Boolean | — |
| SMALLINT / INTEGER / INT | Int | — |
| BIGINT | BigInt | — |
| REAL / FLOAT / DOUBLE PRECISION | Float | — |
| NUMERIC / DECIMAL | Decimal | — |
| VARCHAR / CHARACTER VARYING / TEXT | String | — |
| UUID | String | Uuid |
| JSON / JSONB | Json | — |
| BYTEA | Bytes | — |
| DATE | DateTime | Date |
| TIMESTAMP | DateTime | Timestamp |
| TIMESTAMPTZ / TIMESTAMP WITH TIME ZONE | DateTime | Timestamptz |

Types not in the map are reported as `String` with `unsupported: true`. Array-suffix types (e.g. `TEXT[]`) are flagged as array variants.

For the full type mapping reference see [../type_mapping.md](../type_mapping.md).

---

## Capabilities and limitations

**Supported (confirmed in source):**

- Datasource URL parsing and validation (`parseScratchbirdConnectionUrl`)
- `schema.prisma` text validation (datasource, generator, `env("DATABASE_URL")` requirement)
- ScratchBird-to-Prisma scalar/native type mapping (`mapScratchBirdTypeToPrisma`)
- Metadata-to-`schema.prisma` model generation (schema generator utility in `lib/schema-generator.js`)
- Deterministic reflection round-trip contract helpers (`lib/workflow.js`)
- Migration plan builder for Prisma migration layout (`lib/workflow.js`)
- Full canonical option alias normalization (manager-proxy, token/assertion, channel-binding, dormant-reattach)

**Not yet available / environment-gated (per source README):**

- Full Prisma provider runtime — this package is not a complete Prisma provider
- Live Prisma Client query execution (`findMany`, `create`, etc.) against a running ScratchBird server
- `prisma migrate` CLI integration — migration plan builder provides contract scaffolding only

All connection-level authority is delegated to the ScratchBird Node.js driver and revalidated server-side. The adaptor has `driver_local_authority: advisory_only`.

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
