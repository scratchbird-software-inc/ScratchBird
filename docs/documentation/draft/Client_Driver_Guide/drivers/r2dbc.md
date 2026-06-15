# ScratchBird R2DBC Driver — R2DBC SPI

The ScratchBird R2DBC driver connects reactive Java applications to a
ScratchBird Convergent Data Engine (CDE) through the R2DBC SPI
(Service Provider Interface). It speaks the ScratchBird Native Wire Protocol
(SBWP v1.1) and is designed for non-blocking, back-pressure-capable access
patterns.

**Release status: beta_2 (release_candidate gate)**

> **Draft note:** This driver's source tree contains only a
> `package_contract.json` at this time. No README, build file, or
> implementation source files were present in
> `project/drivers/driver/r2dbc/`. All claims below are sourced from the
> `DriverPackageManifest.csv` row and the `package_contract.json` contract
> file. Implementation-level detail (artifact coordinates, connection URL
> form, code examples) will be added when source is available.

---

## Manifest metadata

| Field | Value |
|---|---|
| `driver_package_uuid` | `019e12a0-0029-7000-8000-000000000029` |
| `api_surface_set` | `r2dbc_spi` |
| `ingress_mode_set` | `direct_listener`, `manager_proxy` |
| `wire_protocol_set` | `sbwp_v1_1` |
| `dsn_key_set` | `database`, `host`, `port`, `user`, `auth_method` |
| `auth_method_set` | `engine_local_password`, `scram_ready` |
| `tls_profile_set` | `scratchbird_tls_1_3_floor` |
| `type_mapping_profile` | `sbsql_core` |
| `diagnostic_mapping_profile` | `native_sqlstate` |
| `metadata_profile` | `sys_information_recursive` |
| `thread_safety_class` | `thread_safe` |
| `pooling_capability` | `reactive_pool` |
| `release_bucket` | `release_candidate` |
| `conformance_profile_ref` | `driver_r2dbc_gate` |

---

## R2DBC SPI surface (from package_contract.json)

The driver implements the following R2DBC SPI types:

| R2DBC type | Role |
|---|---|
| `ConnectionFactoryProvider` | Driver registration; matched by `r2dbc:scratchbird:` URL scheme |
| `ConnectionFactory` | Factory for reactive `Connection` instances |
| `Connection` | Single database connection with reactive lifecycle |
| `Statement` | Prepared or parameterized statement |
| `Result` | Reactive result stream (rows + row metadata) |
| `Row` | Single data row |
| `Batch` | Batch of statements executed as a unit |
| `TransactionDefinition` | Transaction option payload (isolation, read-only, etc.) |
| `ValidationDepth` | Connection validation depth for pool health checks |

---

## Route requirements

The driver enforces the following route requirements:

| Requirement | Description |
|---|---|
| `sbwp_v1_1` | ScratchBird Native Wire Protocol v1.1 |
| `scratchbird_tls_1_3_floor` | TLS 1.3 minimum |
| `engine_authentication_authority` | Auth resolved by the engine |
| `engine_authorization_authority` | Authorization resolved by the engine |
| `mga_transaction_finality` | Transaction finality owned by the MGA transaction inventory |
| `sys_information_metadata` | Metadata served through `sys.information.*` |
| `uuid_identity` | UUID-based catalog identity |
| `no_hidden_replay` | No implicit statement replay |

---

## Conformance profile

The declared conformance areas:

| Area | Included |
|---|---|
| `connect_auth` | Yes |
| `prepare_execute_fetch` | Yes |
| `transactions` | Yes |
| `metadata` | Yes |
| `type_mapping` | Yes |
| `error_mapping` | Yes |
| `reconnect` | Yes |
| `protocol_negotiation` | Yes |
| `cancellation` | Yes |

---

## Connecting (expected URL form)

Based on the manifest `dsn_key_set` (`database`, `host`, `port`, `user`,
`auth_method`), the expected R2DBC URL form is:

```
r2dbc:scratchbird://user:password@host:3092/database
```

Ingress modes `direct_listener` and `manager_proxy` are supported.
TLS floor is `scratchbird_tls_1_3_floor`. Auth methods: `engine_local_password`
and `scram_ready` (covering `PASSWORD`, `SCRAM_SHA_256`, `SCRAM_SHA_512`,
`TOKEN`).

> Confirmed connection API class names and URL parsing behavior are not
> verifiable from the current source tree. The URL form above is inferred
> from the manifest and R2DBC SPI conventions — confirm against driver source
> when available.

---

## Type mapping

The `sbsql_core` type-mapping profile applies (same as Python, JDBC, Go, and
.NET drivers). See [../type_mapping.md](../type_mapping.md) for the canonical
SBsql-to-language-type mapping table.

---

## Metadata and introspection

The metadata profile is `sys_information_recursive`. The driver is expected
to expose `sys.information.*` views through the R2DBC `ConnectionMetadata`
and row-metadata surfaces.

See [../metadata_sys_information.md](../metadata_sys_information.md).

---

## Errors and diagnostics

The diagnostic mapping profile is `native_sqlstate`. R2DBC exceptions carry
SQLSTATE codes through `R2dbcException.getSqlState()`. The retry-boundary
contract (verified for all other ScratchBird drivers) classifies `40001`/`40P01`
as statement-boundary retries and `08xxx` as reconnect-boundary retries.

See [../diagnostics_and_sqlstate.md](../diagnostics_and_sqlstate.md).

---

## Pooling and concurrency

The pooling capability is `reactive_pool`. The driver is `thread_safe`.
Connection pooling is handled reactively, compatible with R2DBC pool libraries
(`r2dbc-pool`). `ValidationDepth` is supported for pool health probes.

See [../pooling_and_concurrency.md](../pooling_and_concurrency.md).

---

## Conformance

Conformance gate: `driver_r2dbc_gate`.

See [../conformance_baseline.md](../conformance_baseline.md).

---

## See also

- [../README.md](../README.md)
- [../connection_and_dsn.md](../connection_and_dsn.md)
- [../authentication.md](../authentication.md)
- [../wire_protocol_sbwp.md](../wire_protocol_sbwp.md)
