# Tableau Adaptor

> **Draft stub — beta_2 / release_candidate.**
> This page describes the Tableau Connector adaptor for ScratchBird.
> Source material is limited to `package_contract.json`; no connector bundle
> file or LookML/TACO source is present in the current tree.
> All claims below are sourced from that file and the
> `DriverPackageManifest.csv` row; nothing is inferred or invented.

## Purpose

The ScratchBird Tableau adaptor integrates ScratchBird — a Convergent Data
Engine (CDE) speaking SBWP v1.1 on default port 3092 — into Tableau Desktop
and Tableau Server.  It presents ScratchBird as a named connector inside
Tableau's connector-plugin system and surfaces four query surfaces to Tableau:
live query, extract refresh, metadata enumeration, and an initial-SQL hook.

Unlike a raw JDBC or ODBC driver loaded through Tableau's generic JDBC/ODBC
path, this adaptor ships as a dedicated Tableau connector plugin (a
`tableau_connector_bundle` artifact).  The adaptor itself does not own a
transport layer: it shapes Tableau connection-dialog, metadata-enumeration,
live-query, and extract-refresh requests and relies on the ScratchBird engine
to enforce authentication, authorization, MGA transaction finality, and schema
identity.  The host runtime is the Tableau connector loader.

**Release status:** beta_2, release_candidate.

---

## Manifest Metadata

| Field | Value |
|---|---|
| `component_id` | `adaptor:scratchbird-tableau` |
| `driver_package_uuid` | `019e12a0-0034-7000-8000-000000000034` |
| `api_surface_set` | `application_adapter` |
| `ingress_mode_set` | `direct_listener`, `manager_proxy` |
| `wire_protocol_set` | `sbwp_v1_1` |
| `dsn_key_set` | `database`, `host`, `port`, `user`, `auth_method` |
| `auth_method_set` | `engine_local_password`, `scram_ready` |
| `tls_profile_set` | `scratchbird_tls_1_3_floor` |
| `type_mapping_profile` | `tableau_mapping` |
| `diagnostic_mapping_profile` | `native_sqlstate` |
| `metadata_profile` | `sys_information_recursive` |
| `thread_safety_class` | `connection_thread_confined` |
| `pooling_capability` | `explicit_session` |
| `release_bucket` | `release_candidate` |
| `conformance_profile_ref` | `adaptor_tableau_gate` |
| Delegates to | none (no underlying driver; host runtime is the Tableau connector) |
| Package type | `tableau_connector_bundle` |
| License | MPL-2.0 |

**Delegation posture:** `explicit_session`.  The adaptor shapes Tableau
requests but does not wrap an underlying ScratchBird driver at the protocol
level.  Authentication authority, MGA transaction finality, UUID identity, and
authorization are all enforced by the ScratchBird engine, not by the adaptor.

---

## API Surfaces

The connector plugin exposes the following Tableau-side surfaces (from
`package_contract.json` `api_surface`):

| Surface | Description |
|---|---|
| `connector_plugin` | The Tableau connector plugin registration |
| `connection_dialog` | Custom connection-dialog fields inside Tableau |
| `metadata_enumeration` | Schema and object discovery shown in Tableau's data source browser |
| `initial_sql` | Initial-SQL hook executed when a connection opens |
| `live_query` | Live-query mode (queries forwarded to ScratchBird at query time) |
| `extract_refresh` | Extract refresh mode (Tableau-side data extract) |

---

## Installation

> **Source note:** The build artifact (`tableau_connector_bundle`) is produced
> by a Tableau packaging gate (`tableau packaging gate`).  The output file
> name is not present in the current source tree (only `package_contract.json`
> exists).  Install steps below are drawn from the package contract; the
> exact `.taco` filename will be specified at release time.

1. Obtain the signed connector bundle (`.taco` file) from the ScratchBird
   release.  SBOM and signing are both gated on the release build; unsigned
   bundles are not supported in production Tableau environments.
2. Place the `.taco` file in the Tableau connector directory:
   - **Tableau Desktop (macOS):** `~/Documents/My Tableau Repository/Connectors/`
   - **Tableau Desktop (Windows):** `Documents\My Tableau Repository\Connectors\`
   - **Tableau Server:** the connectors directory configured for the site, or
     distributed via Tableau Server Manager.
3. Restart Tableau.
4. ScratchBird will appear in the **Connect** panel under **To a Server**.

---

## Configuring a Connection

When creating a new ScratchBird data source in Tableau, fill in the
connection-dialog fields.  These correspond to the `dsn_key_set` declared in
the manifest:

| Field | Manifest key | Notes |
|---|---|---|
| Server | `host` | Hostname or IP address of the ScratchBird server |
| Port | `port` | Default: 3092 |
| Database | `database` | Database name |
| Username | `user` | ScratchBird user principal |
| Password | `auth_method` | `engine_local_password` or `scram_ready` |

TLS is always required: the `scratchbird_tls_1_3_floor` TLS profile enforces
TLS 1.3 as the minimum.  See [../tls_profiles.md](../tls_profiles.md).

Authentication is engine-owned.  The adaptor does not cache, replay, or proxy
credentials.  See [../authentication.md](../authentication.md).

---

## Type Mapping

This adaptor uses the `tableau_mapping` type-mapping profile.  Tableau data
types are mapped from ScratchBird wire types according to that profile.

See [../type_mapping.md](../type_mapping.md) for the full mapping table.

---

## Capabilities and Limitations

| Capability | Status |
|---|---|
| Live query | Supported |
| Extract refresh | Supported |
| Metadata enumeration | Supported |
| Initial SQL | Supported |
| Authentication | Engine-owned (password, SCRAM-ready) |
| MGA transaction finality | Engine-owned; adaptor cannot override |
| Authorization / row-level security | Engine-owned; server revalidates all claims |
| UUID catalog identity | Engine-owned |
| Connection pooling | Explicit session (not pool-managed by adaptor) |
| Direct wire ownership | None — adaptor shapes Tableau requests only |

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
