# Connection and DSN

## Purpose

This page describes how a ScratchBird client opens a session: the two ingress modes available
to all drivers, the shared set of DSN (Data Source Name) keys that all drivers recognize, the
sequence a driver follows to open a connection, and the auth bootstrap flow that runs before
statement execution is possible.

All values on this page are verified from `project/drivers/DriverPackageManifest.csv` and
from driver implementation sources including `project/drivers/driver/python/S1_CONN_IMPLEMENTATION.md`,
`project/drivers/driver/go/S1_CONN_IMPLEMENTATION.md`, and
`project/drivers/tool/cli/cli_auth_bootstrap.cpp`.

This is a **draft**. Components are in `beta_2` / `release_candidate` status.

---

## Ingress Modes

Every ScratchBird driver supports exactly two ingress modes. The mode governs how the client
reaches the server and which front-door accepts the connection.

| Mode | Manifest Value | Description |
| --- | --- | --- |
| Direct listener | `direct_listener` | Client connects directly to a ScratchBird listener endpoint over TCP. The listener negotiates SBWP, authenticates the connection, and routes it to the engine. This is the standard network connection mode. |
| Manager proxy | `manager_proxy` | Client connects through a ScratchBird manager, which acts as a front-door proxy. The client must supply a `manager_auth_token` for the attach; the manager then establishes the engine-side session. This mode is used in managed or multi-tenant deployments. |

Source: `DriverPackageManifest.csv` column `ingress_mode_set` — value `direct_listener;manager_proxy`
for all standard drivers.

The CLI tools document an additional pair of transport modes (`embedded` and `local-ipc`) that map
to IPC transports for local use. Those modes do not use the SBWP TCP listener path. See
[cli_tools.md](cli_tools.md) for details.

---

## Shared DSN Key Set

The following DSN keys are recognized by all standard drivers (those with the `sbsql_core`,
`arrow_recordbatch`, `python_dbapi_mapping`, or `jdbc_mapping` type-mapping profile).

Source: `DriverPackageManifest.csv` column `dsn_key_set`.

### Standard Keys (all drivers except where noted)

| Key | Type | Description |
| --- | --- | --- |
| `database` | string | The target database name or path on the server. Required. |
| `host` | string | Hostname or IP address of the ScratchBird listener or manager. Default: `localhost`. |
| `port` | integer | TCP port. Default: `3092`. |
| `user` | string | Username for authentication. |
| `auth_method` | string | Requested authentication method. See [authentication.md](authentication.md) for admitted values. |

### Alias Keys (Python driver verified; similar aliases apply in other drivers)

The Python driver accepts the following DSN aliases and normalizes them:

| Alias | Canonical Key |
| --- | --- |
| `dbname` | `database` |
| `username` | `user` |
| `connecttimeout` | `connect_timeout` |
| `sockettimeout` | `socket_timeout` |
| `applicationname` | `application_name` |
| `binarytransfer` | `binary_transfer` |

Source: `project/drivers/driver/python/S1_CONN_IMPLEMENTATION.md` — connection config alias
handling.

### ODBC-specific key

The ODBC driver adds a `dsn` key (an ODBC data source name registered in the driver manager)
in addition to the standard keys.

Source: `DriverPackageManifest.csv` row `driver:odbc`, `dsn_key_set` = `database;host;port;dsn;user;auth_method`.

### JDBC-based adaptor keys

Adaptors that embed the JDBC driver (Looker, Hibernate, Metabase, DBeaver) use a `jdbc_url`
key rather than the individual `database;host;port` triple. The `user` and `auth_method` keys
remain.

Source: `DriverPackageManifest.csv` rows for `scratchbird-looker`, `scratchbird-hibernate-dialect`,
`scratchbird-metabase-driver`.

### FlightSQL keys

The FlightSQL driver adds a `flight_endpoint` key alongside `database`, `user`, and `auth_method`.

Source: `DriverPackageManifest.csv` row `driver:flightsql`.

---

## Auth Bootstrap Fields

In addition to the standard DSN keys, drivers that implement the staged auth bootstrap
expose the following startup configuration fields. These are set in the connection config or
DSN and are passed through the startup sequence before the first round-trip.

| Field | Purpose |
| --- | --- |
| `auth_token` | Generic token-auth payload surface |
| `auth_method_id` | Explicit auth method identifier |
| `auth_payload_json` | Auth payload in JSON form |
| `auth_payload_b64` | Auth payload in base64 form |
| `auth_provider_profile` | Auth provider profile selector |

Source: `project/drivers/driver/python/S1_CONN_IMPLEMENTATION.md` — "auth startup config fields".

---

## Opening a Session

The sequence a driver follows to open a session is:

1. **Parse and normalize the DSN or connection config.** Aliases are resolved to canonical keys.
   Non-native `protocol`/`parser`/`dialect` hints (e.g., `jdbc`, `postgresql`, `odbc`) are
   normalized to `native` mode.
2. **Validate ingress mode.** If `front_door_mode=manager_proxy`, a `manager_auth_token` must
   be present before any socket connect attempt. Failure is fast (no socket opened).
3. **Establish the transport.** For `direct_listener`, open a TCP connection to `host:port`
   (default `localhost:3092`). If TLS is required (default unless `sslmode=disable`), perform
   TLS handshake under the `scratchbird_tls_1_3_floor` profile. See [tls_profiles.md](tls_profiles.md).
4. **Negotiate SBWP.** Send a `STARTUP` frame with protocol version `sbwp_v1_1`. See
   [wire_protocol_sbwp.md](wire_protocol_sbwp.md).
5. **Authenticate.** The server responds with an `AuthRequest`. The driver selects an auth
   method from its admitted set and responds. See [authentication.md](authentication.md).
6. **Receive `AuthOk` and `Ready`.** The session is now open and ready for statement execution.

---

## Session Schema

After a session is opened, the active schema context can be queried or changed:

- `get_session_schema()` — returns the normalized active session-schema setting.
- `set_session_schema(schema)` — updates session-schema state and executes `SET SCHEMA` /
  `SET SEARCH_PATH` on the server. Resetting to `None` normalizes to `public`.

Source: `project/drivers/driver/python/S1_CONN_IMPLEMENTATION.md` — "runtime session-schema
parity helpers".

---

## Connection Liveness

Drivers expose a liveness probe:

- `is_valid(timeout_ms)` — returns a boolean backed by a `ping()` round-trip. A closed
  connection returns `False`. A negative timeout raises a `ProgrammingError`.

Source: `project/drivers/driver/python/S1_CONN_IMPLEMENTATION.md` — "JDBC-style liveness helper".

---

## Cross-References

- [authentication.md](authentication.md) — auth method negotiation and credential supply
- [tls_profiles.md](tls_profiles.md) — TLS profile requirements
- [wire_protocol_sbwp.md](wire_protocol_sbwp.md) — SBWP startup and negotiation frames
- [Security Guide: authentication_and_providers.md](../Security_Guide/authentication_and_providers.md) — engine-side provider policy
