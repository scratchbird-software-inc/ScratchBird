# Airbyte Adaptor

> **Draft stub.** The source directory (`project/drivers/adaptor/scratchbird-airbyte/`) contains only `package_contract.json` — no README, no Python source, no connector configuration files, no examples. The information on this page is derived exclusively from `package_contract.json` and the corresponding row in `DriverPackageManifest.csv`.

The Airbyte adaptor (`scratchbird-airbyte`) integrates ScratchBird into the Airbyte data integration platform as a connector. It implements the Airbyte Python CDK connector interface and delegates all wire transport to the ScratchBird Python driver (DB-API 2.0).

**Status:** beta_2 (release_candidate bucket).

**Conformance profile:** `adaptor_airbyte_gate`

---

## Manifest metadata

| Field | Value |
|---|---|
| component\_id | `adaptor:scratchbird-airbyte` |
| driver\_package\_uuid | `019e12a0-0030-7000-8000-000000000030` |
| api\_surface | `application_adapter` |
| ingress\_mode | `direct_listener`, `manager_proxy` |
| delegates to / pooling | `delegates_to_python` (driver:python) |
| type\_mapping\_profile | `python_dbapi_mapping` |
| DSN / connection keys | `database`, `host`, `port`, `user`, `auth_method` |
| auth\_method\_set | `engine_local_password`, `scram_ready` |
| tls\_profile | `scratchbird_tls_1_3_floor` |
| thread\_safety\_class | `connection_thread_confined` |
| diagnostic\_mapping | `native_sqlstate` |
| metadata\_profile | `sys_information_recursive` |
| release\_bucket | `release_candidate` |
| package\_type | `airbyte_connector_bundle` |
| license | MPL-2.0 |

---

## Installation

The artifact is an Airbyte connector bundle, built with:

```bash
python3 project/drivers/scripts/driver_component_runner.py --component adaptor:scratchbird-airbyte
```

Install smoke: Airbyte `spec`/`check`/`discover`/`read` against the staged connector bundle.

No further install instructions are available from source at this time. Connector registration in an Airbyte workspace (self-hosted or cloud) follows standard Airbyte custom connector procedures; specific connector image or package names are not documented in the available source.

---

## API surface (from package_contract.json)

The contract declares the following Airbyte CDK operation surfaces:

| Operation | Description |
|---|---|
| `spec` | Returns the connector configuration schema |
| `check` | Tests connectivity and credential validity |
| `discover` | Returns the Airbyte catalog (available streams) |
| `read` | Reads records from ScratchBird |
| `incremental_state` | Manages incremental sync state checkpointing |
| `destination_write` | Writes records to ScratchBird as a destination |

Catalog metadata maps to ScratchBird `sys.information` views with authorization-filtered visibility, consistent with the `sys_information_recursive` metadata profile.

---

## Authority boundaries

Per the `delegation_posture` in `package_contract.json`:

- The adapter may shape Airbyte catalog and state messages
- The Python driver handles transport and protocol binding
- The server revalidates authentication, authorization, SBLR, UUID, cache, schema, and transaction claims
- MGA transaction finality remains engine-owned

---

## Type and feature mapping

Type mapping follows the `python_dbapi_mapping` profile. See [../type_mapping.md](../type_mapping.md).

---

## Capabilities and limitations

Source coverage is thin (package_contract.json only). The following is known from the contract:

- Both source (read) and destination (write) connector modes are in scope
- Full refresh and incremental sync strategies are in scope
- Comparison baseline is `airbyte-postgres-source`; ScratchBird-specific deltas cover spec, check, discover, full-refresh read, incremental state, and destination write surfaces
- Stream catalog metadata is authorization-filtered via `sys.information`

Connector configuration schema keys, stream selection options, and incremental cursor/bookmark semantics are not yet documented from source.

---

## Diagnostics

SQLSTATE codes follow the `native_sqlstate` profile. See [../diagnostics_and_sqlstate.md](../diagnostics_and_sqlstate.md).

---

## See also

- [../README.md](../README.md) — Client and Driver Guide overview
- [../drivers/python.md](../drivers/python.md) — ScratchBird Python driver (DB-API 2.0)
- [../connection_and_dsn.md](../connection_and_dsn.md) — DSN and connection parameter reference
- [../authentication.md](../authentication.md) — Authentication methods
- [../type_mapping.md](../type_mapping.md) — Full type mapping reference
- [../diagnostics_and_sqlstate.md](../diagnostics_and_sqlstate.md) — SQLSTATE and error mapping
