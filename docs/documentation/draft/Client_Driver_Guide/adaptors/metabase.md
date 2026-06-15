# Metabase Adaptor

> **beta_2 / release_candidate.**
> This page describes the ScratchBird Metabase driver plugin, which enables
> Metabase to connect to ScratchBird via the ScratchBird JDBC driver.

## Purpose

The `scratchbird-metabase-driver` package integrates ScratchBird — a Convergent
Data Engine (CDE) speaking SBWP v1.1 on default port 3092 — into Metabase.  It
is a Metabase plugin written in Clojure that registers `scratchbird` as a
`sql-jdbc` child driver.

The adaptor is intentionally thin: it delegates all transport, authentication,
and protocol binding to the ScratchBird JDBC driver (`driver:jdbc`, artifact
`com.scratchbird/scratchbird-jdbc:0.1.0`) declared in `deps.edn`.  The adaptor
contributes Metabase-layer concerns: connection-property UI fields, JDBC spec
construction, type mapping, date-grain SQL, and feature-support flags.

**Release status:** beta_2, release_candidate.

---

## Manifest Metadata

| Field | Value |
|---|---|
| `component_id` | `adaptor:scratchbird-metabase-driver` |
| `driver_package_uuid` | `019e12a0-0019-7000-8000-000000000019` |
| `api_surface_set` | `application_adapter` |
| `ingress_mode_set` | `driver_embedded_jdbc` |
| `wire_protocol_set` | `sbwp_v1_1` |
| `dsn_key_set` | `jdbc_url`, `user`, `auth_method` |
| `auth_method_set` | `engine_local_password`, `scram_ready` |
| `tls_profile_set` | `scratchbird_tls_1_3_floor` |
| `type_mapping_profile` | `jdbc_mapping` |
| `diagnostic_mapping_profile` | `native_sqlstate` |
| `metadata_profile` | `sys_information_recursive` |
| `thread_safety_class` | `connection_thread_confined` |
| `pooling_capability` | `delegates_to_jdbc` |
| `release_bucket` | `release_candidate` |
| `conformance_profile_ref` | `adaptor_metabase_gate` |
| Delegates to | `driver:jdbc` (`com.scratchbird/scratchbird-jdbc:0.1.0`) |
| Package type | `metabase_plugin_jar` |
| Build command | `clojure -T:build jar` |
| Output artifact | `target/scratchbird-metabase-driver-0.1.0.jar` |
| License | MPL-2.0 |

**Delegation posture:** `delegates_to_jdbc`.  The adaptor shapes Metabase
feature flags, metadata, and HoneySQL query requests; the JDBC driver handles
all transport and protocol binding.  Authentication authority, MGA transaction
finality, authorization, and UUID identity remain engine-owned.

---

## Plugin Manifest

The plugin is declared in `metabase-plugin.yaml`:

```yaml
info:
  name: ScratchBird
  version: 0.1.0
  description: ScratchBird JDBC driver for Metabase

driver:
  name: scratchbird
  init: metabase.driver.scratchbird/init
  parent: sql-jdbc
```

The `parent: sql-jdbc` declaration means the driver inherits all Metabase
`sql-jdbc` behaviour and overrides only what is ScratchBird-specific.

---

## Clojure Namespaces

| Namespace | File | Role |
|---|---|---|
| `metabase.driver.scratchbird` | `src/metabase/driver/scratchbird.clj` | Driver registration, `connection-details->spec`, `can-connect?`, `db-default-timezone`, `database-supports?`, type-mapping dispatch, HoneySQL date functions |
| `metabase.driver.scratchbird-support` | `src/metabase/driver/scratchbird_support.clj` | Pure config: type map, feature flags, connection-property definitions, JDBC property builder |

The JDBC driver class is `com.scratchbird.jdbc.SBDriver`, with subprotocol
`scratchbird`.  JDBC URLs are constructed as:

```
jdbc:scratchbird://host:port/database
```

---

## Installation

### Build the Plugin JAR

```bash
cd project/drivers/adaptor/scratchbird-metabase-driver
clojure -T:build jar
# Output: target/scratchbird-metabase-driver-0.1.0.jar
```

This bundles `metabase-plugin.yaml`, the compiled Clojure namespaces, and the
ScratchBird JDBC JAR into a single plugin JAR.

Expected JAR layout:

```
scratchbird-metabase-driver-0.1.0.jar
├── metabase-plugin.yaml
└── metabase/driver/scratchbird.clj   (compiled)
```

### Install in Metabase

1. Copy `target/scratchbird-metabase-driver-0.1.0.jar` to `MB_PLUGINS_DIR`
   (the directory Metabase scans for plugin JARs, typically `./plugins/`).
2. Restart Metabase.
3. ScratchBird will appear as a database option in the **Add Database** dialog.

> Update the `com.scratchbird/scratchbird-jdbc` version in `deps.edn` to match
> your ScratchBird release version before building.

---

## Configuring a Connection in Metabase

When adding ScratchBird in Metabase's **Add Database** dialog, the following
fields are presented (sourced from `scratchbird-support.clj`
`scratchbird-connection-properties`):

| Field | Key | Type | Default | Notes |
|---|---|---|---|---|
| Host | `host` | string | `localhost` | Hostname or IP address |
| Port | `port` | integer | `3092` | Default ScratchBird port |
| Database | `db` | string | — | Database name |
| Username | `user` | string | — | ScratchBird user principal |
| Password | `password` | password | — | |
| SSL Mode | `sslmode` | select | `require` | `disable`, `allow`, `prefer`, `require`, `verify-ca`, `verify-full` |
| CA Certificate | `sslrootcert` | string | — | Path to CA certificate file |
| Client Certificate | `sslcert` | string | — | Path to client certificate |
| Client Key | `sslkey` | string | — | Path to client key |
| SSL Key Password | `sslpassword` | password | — | |
| Role | `role` | string | — | Optional role to SET on connect |
| Current Schema | `currentSchema` | string | — | Optional schema override; if omitted, server default applies (`users.public` fallback) |

Additional JDBC fields exposed by the adaptor (from README): `search_path`,
manager-proxy fields (`managerAuthToken`, etc.), staged auth/bootstrap,
workload-identity, proxy-assertion, and dormant-reattach options.

TLS is always required in production (`sslmode=require` default,
`scratchbird_tls_1_3_floor`).  See [../tls_profiles.md](../tls_profiles.md).

Authentication is engine-owned.  See [../authentication.md](../authentication.md).

---

## Type Mapping

The adaptor maps ScratchBird database types to Metabase base types in
`scratchbird-support.clj`.  Representative mappings:

| ScratchBird type | Metabase base type |
|---|---|
| `BOOLEAN` | `:type/Boolean` |
| `SMALLINT`, `INTEGER`, `INT` | `:type/Integer` |
| `BIGINT`, `INT8` | `:type/BigInteger` |
| `REAL`, `FLOAT`, `DOUBLE` | `:type/Float` |
| `NUMERIC`, `DECIMAL` | `:type/Decimal` |
| `CHAR`, `VARCHAR`, `TEXT` | `:type/Text` |
| `DATE` | `:type/Date` |
| `TIME` | `:type/Time` |
| `TIMESTAMP` | `:type/DateTime` |
| `TIMESTAMPTZ` / `TIMESTAMP WITH TIME ZONE` | `:type/DateTimeWithTZ` |
| `UUID` | `:type/UUID` |
| `JSON`, `JSONB`, `VARIANT` | `:type/JSON` |
| `ARRAY`, `VECTOR` | `:type/Array` |
| `INET`, `CIDR` | `:type/IPAddress` |

This adaptor uses the `jdbc_mapping` type-mapping profile.
See [../type_mapping.md](../type_mapping.md) for the full profile.

---

## Feature Flags

The adaptor declares which Metabase features ScratchBird supports via
`scratchbird-feature-support` in `scratchbird_support.clj`.  Selected flags:

| Feature | Supported |
|---|---|
| Foreign keys | Yes |
| Schemas | Yes |
| Basic / standard-deviation / expression / percentile aggregations | Yes |
| Expressions (today, datetime, date, integer, float, text) | Yes |
| Window functions (cumulative, offset) | Yes |
| Regex | Yes (lookaheads/lookbehinds: No) |
| Collate | Yes |
| UUID type | Yes |
| Key constraints / describe-fields / describe-indexes | Yes |
| Table privileges | No |
| Nested field columns | No |
| Uploads / upload-with-auto-pk | No |
| Multiple databases per connection | No |

---

## Diagnostics

SQLSTATE codes use the `native_sqlstate` diagnostic mapping profile, surfaced
through the JDBC driver.
See [../diagnostics_and_sqlstate.md](../diagnostics_and_sqlstate.md).

Test suite: `clojure -T:build jar` (build smoke); `metabase_support_contract_smoke`.

---

## See Also

- [../README.md](../README.md) — Client and Driver Guide overview
- [../drivers/jdbc.md](../drivers/jdbc.md) — ScratchBird JDBC driver (underlying driver)
- [../connection_and_dsn.md](../connection_and_dsn.md) — DSN and connection-string reference
- [../authentication.md](../authentication.md) — Authentication methods
- [../tls_profiles.md](../tls_profiles.md) — TLS profiles
- [../type_mapping.md](../type_mapping.md) — Type mapping profiles
- [../diagnostics_and_sqlstate.md](../diagnostics_and_sqlstate.md) — Diagnostics and SQLSTATE
