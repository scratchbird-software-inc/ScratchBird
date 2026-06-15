# Looker Adaptor

> **Draft stub — beta_2 / release_candidate.**
> This page describes the Looker JDBC adaptor for ScratchBird.
> Source material is limited to `package_contract.json`; no LookML dialect
> source or Looker connection-profile files are present in the current tree.
> All claims are sourced from that file and the `DriverPackageManifest.csv`
> row; nothing is inferred or invented.

## Purpose

The ScratchBird Looker adaptor integrates ScratchBird — a Convergent Data
Engine (CDE) speaking SBWP v1.1 on default port 3092 — into Google Looker
(formerly Looker Studio Enterprise / Looker Platform).  It registers
ScratchBird as a JDBC-backed Looker database dialect and exposes LookML
connection profiles, SQL Runner, LookML SQL table naming, explore metadata, and
persistent derived tables (PDTs).

Unlike Tableau or Power BI, the Looker adaptor delegates transport entirely to
the ScratchBird JDBC driver (`driver:jdbc`).  The adaptor itself is a thin
`looker_jdbc_adapter_bundle` that configures Looker's JDBC integration layer;
all network I/O, protocol negotiation, and authentication flow through the JDBC
driver.

**Release status:** beta_2, release_candidate.

---

## Manifest Metadata

| Field | Value |
|---|---|
| `component_id` | `adaptor:scratchbird-looker` |
| `driver_package_uuid` | `019e12a0-0032-7000-8000-000000000032` |
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
| `conformance_profile_ref` | `adaptor_looker_gate` |
| Delegates to | `driver:jdbc` (ScratchBird JDBC driver) |
| Package type | `looker_jdbc_adapter_bundle` |
| License | MPL-2.0 |

**Delegation posture:** `delegates_to_jdbc`.  The adaptor shapes Looker
LookML connection-profile and PDT metadata requests; the JDBC driver handles
all transport and protocol binding.  Authentication authority, MGA transaction
finality, authorization, and UUID identity remain engine-owned.  The server
revalidates all incoming command and metadata claims.

---

## API Surfaces

The Looker adaptor exposes the following surfaces (from
`package_contract.json` `api_surface`):

| Surface | Description |
|---|---|
| `connection_profile` | Looker database connection profile configuration |
| `sql_runner` | SQL Runner queries executed against ScratchBird |
| `lookml_sql_table_name` | LookML `sql_table_name` references resolved against ScratchBird schemas |
| `explore_metadata` | Explore-level metadata enumeration |
| `persistent_derived_table` | PDT creation and management through Looker's PDT lifecycle |

---

## Installation

> **Source note:** The bundle artifact is produced by the component runner
> script:
> ```
> python3 project/drivers/scripts/driver_component_runner.py --component adaptor:scratchbird-looker
> ```
> Only `package_contract.json` is present in the current tree.  The install
> steps below are drawn from the package contract and the Looker JDBC
> integration pattern; the exact bundle filename is specified at release time.

1. Build or obtain the `looker_jdbc_adapter_bundle` from the ScratchBird
   release.
2. Obtain the ScratchBird JDBC driver JAR (see [../drivers/jdbc.md](../drivers/jdbc.md)).
   The bundle embeds (or references) this JAR.
3. Place the JDBC JAR in Looker's JDBC driver directory and register the
   ScratchBird connection profile in Looker's database dialect configuration.
4. Restart Looker.

> SBOM and signing are both gated on the release build.

---

## Configuring a Connection

The Looker adaptor uses a `jdbc_url`-based connection (not individual
host/port/database keys at the Looker UI level).  Provide:

| Field | Manifest key | Notes |
|---|---|---|
| JDBC URL | `jdbc_url` | `jdbc:scratchbird://host:3092/database?sslmode=require` |
| Username | `user` | ScratchBird user principal |
| Password | `auth_method` | `engine_local_password` or `scram_ready` |

Example JDBC URL:

```
jdbc:scratchbird://db.example.com:3092/analytics?sslmode=require
```

TLS is always required: the `scratchbird_tls_1_3_floor` TLS profile enforces
TLS 1.3 as the minimum.  Additional JDBC properties (TLS certificates, schema
override, manager-proxy fields, etc.) may be appended as URL query parameters
or passed as connection properties; see [../drivers/jdbc.md](../drivers/jdbc.md)
for the full JDBC property reference.

Authentication is engine-owned.  See [../authentication.md](../authentication.md).

---

## Type Mapping

This adaptor uses the `jdbc_mapping` type-mapping profile (inherited from the
underlying JDBC driver).  See [../type_mapping.md](../type_mapping.md) for
the full mapping table.

---

## Capabilities and Limitations

| Capability | Status |
|---|---|
| SQL Runner | Supported |
| LookML sql_table_name | Supported |
| Explore metadata | Supported |
| Persistent derived tables (PDTs) | Supported |
| Authentication | Engine-owned via JDBC driver (password, SCRAM-ready) |
| MGA transaction finality | Engine-owned; adaptor cannot override |
| Authorization / row-level security | Engine-owned; server revalidates all claims |
| PDT and metadata server auth boundaries | Preserved (per package contract) |
| UUID catalog identity | Engine-owned |
| Transport / protocol | Delegated to ScratchBird JDBC driver |

---

## Diagnostics

SQLSTATE codes use the `native_sqlstate` diagnostic mapping profile, surfaced
through the JDBC driver.
See [../diagnostics_and_sqlstate.md](../diagnostics_and_sqlstate.md).

---

## See Also

- [../README.md](../README.md) — Client and Driver Guide overview
- [../drivers/jdbc.md](../drivers/jdbc.md) — ScratchBird JDBC driver (underlying driver)
- [../connection_and_dsn.md](../connection_and_dsn.md) — DSN and connection-string reference
- [../authentication.md](../authentication.md) — Authentication methods
- [../tls_profiles.md](../tls_profiles.md) — TLS profiles
- [../type_mapping.md](../type_mapping.md) — Type mapping profiles
- [../diagnostics_and_sqlstate.md](../diagnostics_and_sqlstate.md) — Diagnostics and SQLSTATE
