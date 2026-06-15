# ScratchBird FlightSQL Driver — Arrow Flight SQL over gRPC

The ScratchBird FlightSQL driver gives applications access to a
ScratchBird Convergent Data Engine (CDE) through the Arrow Flight SQL
protocol over gRPC. Queries are dispatched as Flight SQL RPC calls;
result data is streamed back as Arrow `RecordBatch` payloads carried in
Flight `DoGet` streams. The driver is the standard integration path for
clients that consume the Apache Arrow Flight SQL specification, including
ADBC Flight SQL backends, Substrait-based query planners, and gRPC-native
analytics tools.

The underlying wire protocol remains SBWP v1.1 (ScratchBird Native Wire
Protocol). gRPC Status codes are mapped to SQLSTATE using the
`grpc_status_sqlstate` diagnostic profile.

**Release status: beta_2 (release_candidate gate)**

> **Draft stub.** The `project/drivers/driver/flightsql/` source tree
> contains only `package_contract.json` at the time of this writing. No
> README, CMakeLists, proto files, or implementation source are present.
> All sections below reflect only what is verifiable from that contract
> and the shared DriverPackageManifest. Sections that would require
> implementation source have been omitted rather than invented.

---

## Manifest metadata

| Field | Value |
|---|---|
| `component_id` | `driver:flightsql` |
| `driver_package_uuid` | `019e12a0-0026-7000-8000-000000000026` |
| `driver_family` | `flight_sql` |
| `api_surface_set` | `flight_sql_grpc` |
| `ingress_mode_set` | `direct_listener`, `manager_proxy` |
| `wire_protocol_set` | `sbwp_v1_1` |
| `dsn_key_set` | `flight_endpoint`, `database`, `user`, `auth_method` |
| `auth_method_set` | `engine_local_password`, `scram_ready` |
| `tls_profile_set` | `scratchbird_tls_1_3_floor` |
| `type_mapping_profile` | `arrow_recordbatch` |
| `diagnostic_mapping_profile` | `grpc_status_sqlstate` |
| `metadata_profile` | `sys_information_recursive` |
| `thread_safety_class` | `thread_safe` |
| `pooling_capability` | `stream_pool` |
| `release_bucket` | `release_candidate` |
| `conformance_profile_ref` | `driver_flightsql_gate` |

---

## Installation

> Source tree: `project/drivers/driver/flightsql/`
>
> No build manifest (CMakeLists.txt, pom.xml, go.mod, or equivalent) is
> present in the current source snapshot. The artifact name, build
> instructions, and system requirements cannot be stated without that
> file. This section will be completed when the build manifest is
> committed.

---

## Flight SQL RPC surface

The `package_contract.json` declares the following Flight SQL gRPC
operations for this driver:

| RPC | Role |
|---|---|
| `GetFlightInfo` | Plans a query and returns `FlightInfo` with one or more `FlightEndpoint` locations. |
| `DoGet` | Streams result data as Arrow `RecordBatch` payloads for a `Ticket` from `GetFlightInfo`. |
| `GetTables` | Returns table metadata; corresponds to `CommandGetTables` in the Flight SQL spec. |
| `GetSqlInfo` | Returns server capability flags; corresponds to `CommandGetSqlInfo`. |
| `BeginTransaction` | Opens an MGA-engine transaction on the Flight SQL session. |
| `EndTransaction` | Commits or rolls back the active transaction. |
| `CreatePreparedStatement` | Prepares a parameterised statement; returns a `PreparedStatementHandle`. |
| `Execute` | Executes a prepared statement with bound parameters. |

These operations follow the Arrow Flight SQL specification. Refer to the
upstream Arrow Flight SQL specification for full message and RPC
definitions.

---

## Connecting

The manifest records the DSN key `flight_endpoint` in addition to the
standard `database`, `user`, and `auth_method` keys. The `flight_endpoint`
key carries the gRPC endpoint address (host and port) of the ScratchBird
Flight SQL listener.

> Concrete connection examples (channel options, metadata headers, TLS
> certificate configuration) cannot be stated without implementation
> source confirming the exact client constructor used by this driver.
> This section will be completed when implementation source is committed.

Both `direct_listener` and `manager_proxy` ingress modes are listed in
the manifest. TLS floor: `scratchbird_tls_1_3_floor` (TLS 1.3 minimum).

---

## Results — Arrow RecordBatch streams

The `type_mapping_profile` is `arrow_recordbatch`. ScratchBird SBsql
types are projected onto Arrow logical types when the driver streams
`RecordBatch` payloads back from `DoGet`. The result of `GetFlightInfo`
contains `FlightEndpoint` references that the client then fetches with
`DoGet`.

See [../type_mapping.md](../type_mapping.md) for the full SBsql-to-Arrow
column type table.

---

## Metadata

Schema, table, and column metadata are available through the Flight SQL
`GetTables` and related `CommandGetXxx` operations, backed by
`sys.information.*` views on the server (metadata profile:
`sys_information_recursive`).

See [../metadata_sys_information.md](../metadata_sys_information.md) for
the view hierarchy and query patterns.

---

## Errors and diagnostics

The diagnostic mapping profile is `grpc_status_sqlstate`. gRPC `Status`
codes returned by the server are mapped to SQLSTATE codes following the
ScratchBird diagnostic classification. The gRPC `Status.details` field
carries additional ScratchBird diagnostic fields where available.

| gRPC Status | SQLSTATE class |
|---|---|
| `OK` | — (no error) |
| `INVALID_ARGUMENT` | `22xxx` (data exception) |
| `NOT_FOUND` | `42xxx` (syntax error or access rule) |
| `ALREADY_EXISTS` | `23xxx` (integrity constraint) |
| `PERMISSION_DENIED` | `28xxx` (invalid authorization) |
| `UNAUTHENTICATED` | `28000` |
| `UNAVAILABLE` | `08xxx` (connection exception) |
| `ABORTED` | `40xxx` (transaction rollback) |
| Other | Mapped per `grpc_status_sqlstate` profile |

> The table above reflects the `grpc_status_sqlstate` profile name from
> the manifest. The detailed per-code mapping is defined in the shared
> diagnostics chapter rather than driver source (which is absent).

See [../diagnostics_and_sqlstate.md](../diagnostics_and_sqlstate.md) for
the full SQLSTATE classification and retry guidance.

---

## Pooling and concurrency

The manifest records `thread_safe` thread-safety and `stream_pool`
pooling capability. A stream pool manages the lifecycle of active `DoGet`
streams, allowing concurrent result consumption across multiple callers
without opening redundant gRPC channels.

Concrete pool configuration parameters cannot be stated without
implementation source.

See [../pooling_and_concurrency.md](../pooling_and_concurrency.md) for
ScratchBird pooling concepts and stream pool semantics.

---

## Conformance

Conformance profile reference: `driver_flightsql_gate`.

The manifest lists the following conformance areas:
`connect_auth`, `prepare_execute_fetch`, `transactions`, `metadata`,
`type_mapping`, `error_mapping`, `reconnect`, `protocol_negotiation`,
`cancellation`.

See [../conformance_baseline.md](../conformance_baseline.md) for the
baseline conformance definition.

---

## Route requirements

The driver declares the following route requirements in its package
contract:

| Requirement | Purpose |
|---|---|
| `sbwp_v1_1` | Wire protocol version floor |
| `scratchbird_tls_1_3_floor` | Minimum TLS version |
| `engine_authentication_authority` | Engine handles auth |
| `engine_authorization_authority` | Engine handles authz |
| `mga_transaction_finality` | MGA-based transaction model |
| `sys_information_metadata` | Metadata via `sys.information.*` |
| `uuid_identity` | UUID-based catalog identity |
| `no_hidden_replay` | Explicit retry semantics only |

---

## See also

- [../README.md](../README.md) — Client and Driver Guide overview
- [../connection_and_dsn.md](../connection_and_dsn.md) — DSN reference
- [../authentication.md](../authentication.md) — Auth methods
- [../wire_protocol_sbwp.md](../wire_protocol_sbwp.md) — SBWP v1.1 wire protocol
- [../type_mapping.md](../type_mapping.md) — SBsql-to-Arrow type mapping
- [../diagnostics_and_sqlstate.md](../diagnostics_and_sqlstate.md) — SQLSTATE / gRPC status reference
- [../pooling_and_concurrency.md](../pooling_and_concurrency.md) — Stream pool
- [../conformance_baseline.md](../conformance_baseline.md) — Conformance baseline
