# Power BI Adaptor

> **Draft stub — beta_2 / release_candidate.**
> This page describes the Power BI (Power Query) connector adaptor for ScratchBird.
> Source material is limited to `package_contract.json`; no `.pq` or `.mez`
> connector source is present in the current tree.
> All claims are sourced from that file and the `DriverPackageManifest.csv`
> row; nothing is inferred or invented.

## Purpose

The ScratchBird Power BI adaptor integrates ScratchBird — a Convergent Data
Engine (CDE) speaking SBWP v1.1 on default port 3092 — into Power BI
Desktop and Power BI Service.  It registers ScratchBird as a custom Power Query
connector and exposes DirectQuery mode, Import mode, a NavigationTable for
schema browsing, and a query-folding profile.

The adaptor ships as a Power Query connector artifact (`powerquery_connector`,
typically a `.mez` file compiled from M-language sources).  It does not embed
an underlying ScratchBird driver at the protocol level: the host runtime is the
Power Query connector loader.  Authentication, authorization, MGA transaction
finality, and schema identity are enforced by the ScratchBird engine; the
adaptor shapes navigation and folding requests and never bypasses server
revalidation.

**Release status:** beta_2, release_candidate.

---

## Manifest Metadata

| Field | Value |
|---|---|
| `component_id` | `adaptor:scratchbird-powerbi` |
| `driver_package_uuid` | `019e12a0-0033-7000-8000-000000000033` |
| `api_surface_set` | `application_adapter` |
| `ingress_mode_set` | `direct_listener`, `manager_proxy` |
| `wire_protocol_set` | `sbwp_v1_1` |
| `dsn_key_set` | `database`, `host`, `port`, `user`, `auth_method` |
| `auth_method_set` | `engine_local_password`, `scram_ready` |
| `tls_profile_set` | `scratchbird_tls_1_3_floor` |
| `type_mapping_profile` | `powerquery_mapping` |
| `diagnostic_mapping_profile` | `native_sqlstate` |
| `metadata_profile` | `sys_information_recursive` |
| `thread_safety_class` | `connection_thread_confined` |
| `pooling_capability` | `explicit_session` |
| `release_bucket` | `release_candidate` |
| `conformance_profile_ref` | `adaptor_powerbi_gate` |
| Delegates to | none (no underlying driver; host runtime is the Power Query connector) |
| Package type | `powerquery_connector` |
| License | MPL-2.0 |

**Delegation posture:** `explicit_session`.  The adaptor shapes Power Query
navigation and folding requests but does not own a transport layer.
Authentication authority, MGA transaction finality, UUID identity, and
authorization remain engine-owned.  The package contract explicitly requires
that query folding never bypasses server revalidation or
authorization-filtered metadata.

---

## API Surfaces

The Power Query connector exposes the following surfaces (from
`package_contract.json` `api_surface`):

| Surface | Description |
|---|---|
| `PowerQueryConnector` | The connector registration in Power Query |
| `NavigationTable` | Schema/table browser shown in Power BI's Navigator pane |
| `DirectQuery` | Live DirectQuery mode — queries execute against ScratchBird at report time |
| `ImportMode` | Import mode — data is loaded into the Power BI model |
| `credential_kind_username_password` | Username + password credential kind |
| `folding_profile` | Query-folding configuration (determines which M operations fold to SQL) |

---

## Installation

> **Source note:** The signed connector file (`.mez`) is produced by a Power
> Query packaging gate.  The output filename is not present in the current
> source tree (only `package_contract.json` exists).  Install steps below
> follow the package contract; the exact filename is specified at release time.

### Power BI Desktop

1. Obtain the signed `.mez` connector file from the ScratchBird release.
2. Place it in:
   - **Windows:** `Documents\Power BI Desktop\Custom Connectors\`
3. In Power BI Desktop, go to **File → Options and settings → Options →
   Security** and set **Data extensions** to allow any extension or to allow
   the specific connector (depending on your security posture).
4. Restart Power BI Desktop.
5. ScratchBird will appear in the **Get Data** connector list.

### Power BI Service (via On-premises Data Gateway)

1. Place the `.mez` file in the gateway's custom-connectors folder (configured
   in the gateway settings).
2. Restart the gateway service.
3. Datasets using the ScratchBird connector can now be refreshed through the
   gateway.

> SBOM and signing are both gated on the release build.

---

## Configuring a Connection

When adding ScratchBird as a data source in Power BI, fill in the following
fields (corresponding to the `dsn_key_set`):

| Field | Manifest key | Notes |
|---|---|---|
| Server | `host` | Hostname or IP address of the ScratchBird server |
| Port | `port` | Default: 3092 |
| Database | `database` | Database name |
| Username | `user` | ScratchBird user principal |
| Password | `auth_method` | `engine_local_password` or `scram_ready` |

TLS is always required: the `scratchbird_tls_1_3_floor` TLS profile enforces
TLS 1.3 as the minimum.  See [../tls_profiles.md](../tls_profiles.md).

Authentication is engine-owned.  See [../authentication.md](../authentication.md).

---

## Type Mapping

This adaptor uses the `powerquery_mapping` type-mapping profile.  ScratchBird
wire types are mapped to Power Query data types according to that profile.

See [../type_mapping.md](../type_mapping.md) for the full mapping table.

---

## Capabilities and Limitations

| Capability | Status |
|---|---|
| DirectQuery mode | Supported |
| Import mode | Supported |
| NavigationTable (schema browser) | Supported |
| Query folding | Supported via `folding_profile` |
| Username/password credential | Supported |
| Authentication | Engine-owned (password, SCRAM-ready) |
| MGA transaction finality | Engine-owned; adaptor cannot override |
| Authorization / filtered metadata | Engine-owned; server revalidates all claims |
| Query folding bypass of server auth | Explicitly prohibited by package contract |
| UUID catalog identity | Engine-owned |
| Connection pooling | Explicit session (not pool-managed by adaptor) |
| Direct wire ownership | None — adaptor shapes Power Query requests only |

---

## Diagnostics

SQLSTATE codes use the `native_sqlstate` diagnostic mapping profile.
See [../diagnostics_and_sqlstate.md](../diagnostics_and_sqlstate.md).

---

## See Also

- [../README.md](../README.md) — Client and Driver Guide overview
- [../connection_and_dsn.md](../connection_and_dsn.md) — DSN and connection-string reference
- [../authentication.md](../authentication.md) — Authentication methods
- [../tls_profiles.md](../tls_profiles.md) — TLS profiles
- [../type_mapping.md](../type_mapping.md) — Type mapping profiles
- [../diagnostics_and_sqlstate.md](../diagnostics_and_sqlstate.md) — Diagnostics and SQLSTATE
