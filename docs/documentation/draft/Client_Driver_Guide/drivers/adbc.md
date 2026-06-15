# ScratchBird ADBC Driver — Arrow Database Connectivity (ADBC C API)

The ScratchBird ADBC driver gives Arrow-native applications access to a
ScratchBird Convergent Data Engine (CDE) through the Arrow Database
Connectivity (ADBC) C API specification. Query results are returned as
Arrow `RecordBatch` streams rather than row-by-row result sets, which
makes the driver the natural choice for high-throughput analytical
pipelines, columnar compute engines, and tools in the Apache Arrow
ecosystem.

The driver speaks the ScratchBird Native Wire Protocol (SBWP v1.1)
directly and does not sit on top of ODBC, JDBC, or any other
intermediate layer.

**Release status: beta_2 (release_candidate gate)**

> **Draft stub.** The `project/drivers/driver/adbc/` source tree
> contains only `package_contract.json` at the time of this writing. No
> README, CMakeLists, header files, or implementation source are present.
> All sections below reflect only what is verifiable from that contract
> and the shared DriverPackageManifest. Sections that would require
> implementation source have been omitted rather than invented.

---

## Manifest metadata

| Field | Value |
|---|---|
| `component_id` | `driver:adbc` |
| `driver_package_uuid` | `019e12a0-0025-7000-8000-000000000025` |
| `driver_family` | `adbc` |
| `api_surface_set` | `adbc_c_api` |
| `ingress_mode_set` | `direct_listener`, `manager_proxy` |
| `wire_protocol_set` | `sbwp_v1_1` |
| `dsn_key_set` | `database`, `host`, `port`, `user`, `auth_method` |
| `auth_method_set` | `engine_local_password`, `scram_ready` |
| `tls_profile_set` | `scratchbird_tls_1_3_floor` |
| `type_mapping_profile` | `arrow_recordbatch` |
| `diagnostic_mapping_profile` | `native_sqlstate` |
| `metadata_profile` | `sys_information_recursive` |
| `thread_safety_class` | `thread_safe` |
| `pooling_capability` | `connection_pool` |
| `release_bucket` | `release_candidate` |
| `conformance_profile_ref` | `driver_adbc_gate` |

---

## Installation

> Source tree: `project/drivers/driver/adbc/`
>
> No build manifest (CMakeLists.txt, Makefile, or equivalent) is present
> in the current source snapshot. The artifact name, build instructions,
> and system requirements cannot be stated without that file. This section
> will be completed when the build manifest is committed.

---

## API objects (ADBC C API)

The `package_contract.json` declares the following ADBC C API surface
objects for this driver:

| Symbol | Role |
|---|---|
| `AdbcDatabase` | Holds driver-level configuration (host, port, database, credentials). |
| `AdbcConnection` | Represents an authenticated session; wraps one SBWP v1.1 transport connection. |
| `AdbcStatement` | Owns a prepared or ad-hoc query and its execution state. |
| `ArrowArrayStream` | The result-set carrier; rows are consumed as Arrow `RecordBatch` chunks. |
| `catalog_objects` | Exposes catalog/schema/table enumeration through the ADBC metadata API. |
| `bulk_ingest` | Provides Arrow-native bulk data ingest (Arrow `RecordBatch` in, table append/replace out). |

These symbols follow the ADBC C API specification. Refer to the upstream
ADBC specification for the full function signatures (`AdbcDatabaseNew`,
`AdbcConnectionNew`, `AdbcStatementNew`, etc.).

---

## Connecting

> Connection form and minimal example cannot be stated without
> implementation source confirming the exact `AdbcDatabase` key names
> used by this driver. The manifest records the DSN key set as
> `database`, `host`, `port`, `user`, `auth_method`. Expected connection
> form (subject to verification once source is committed):
>
> ```c
> // Illustrative only — key names unverified without implementation source
> AdbcDatabase db = {0};
> AdbcDatabaseNew(&db, &error);
> AdbcDatabaseSetOption(&db, "host",     "localhost",    &error);
> AdbcDatabaseSetOption(&db, "port",     "3092",         &error);
> AdbcDatabaseSetOption(&db, "database", "mydb",         &error);
> AdbcDatabaseSetOption(&db, "user",     "myuser",       &error);
> AdbcDatabaseSetOption(&db, "password", "mypass",       &error);
> AdbcDatabaseInit(&db, &error);
> ```
>
> This example must be verified against the driver header / implementation
> before publishing.

Both `direct_listener` and `manager_proxy` ingress modes are listed in
the manifest. TLS floor: `scratchbird_tls_1_3_floor` (TLS 1.3 minimum).

---

## Results — Arrow RecordBatch

The `type_mapping_profile` is `arrow_recordbatch`. ScratchBird SBsql
types are projected onto Arrow logical types when the driver returns
results. The concrete Arrow type used for each SBsql type is defined in
the shared type mapping reference.

See [../type_mapping.md](../type_mapping.md) for the full SBsql-to-Arrow
column type table.

---

## Metadata

Schema, table, and column metadata are available through the ADBC
`catalog_objects` surface, backed by `sys.information.*` views on the
server (metadata profile: `sys_information_recursive`).

See [../metadata_sys_information.md](../metadata_sys_information.md) for
the view hierarchy and query patterns.

---

## Errors and diagnostics

The diagnostic mapping profile is `native_sqlstate`. Errors are surfaced
through the ADBC `AdbcError` struct; the `sqlstate` field carries a
five-character SQLSTATE code following the ScratchBird native SQLSTATE
classification.

See [../diagnostics_and_sqlstate.md](../diagnostics_and_sqlstate.md) for
the SQLSTATE class map and retry guidance.

---

## Pooling and concurrency

The manifest records `thread_safe` thread-safety and `connection_pool`
pooling capability. Concrete pool configuration parameters cannot be
stated without implementation source.

See [../pooling_and_concurrency.md](../pooling_and_concurrency.md) for
ScratchBird pooling concepts.

---

## Conformance

Conformance profile reference: `driver_adbc_gate`.

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
- [../diagnostics_and_sqlstate.md](../diagnostics_and_sqlstate.md) — SQLSTATE reference
- [../pooling_and_concurrency.md](../pooling_and_concurrency.md) — Pooling
- [../conformance_baseline.md](../conformance_baseline.md) — Conformance baseline
