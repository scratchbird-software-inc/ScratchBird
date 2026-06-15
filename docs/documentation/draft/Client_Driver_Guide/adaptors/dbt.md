# dbt Adaptor

> **Draft stub.** The source directory (`project/drivers/adaptor/scratchbird-dbt-adapter/`) contains only `package_contract.json` — no README, no Python source, no `pyproject.toml`, no examples. The information on this page is derived exclusively from `package_contract.json` and the corresponding row in `DriverPackageManifest.csv`.

The dbt adaptor (`scratchbird-dbt-adapter`) integrates ScratchBird into the dbt data transformation framework. It provides a dbt adapter plugin that compiles dbt relation and materialization requests against ScratchBird and delegates all wire transport to the ScratchBird Python driver (DB-API 2.0).

**Status:** beta_2 (release_candidate bucket).

**Conformance profile:** `adaptor_dbt_gate`

---

## Manifest metadata

| Field | Value |
|---|---|
| component\_id | `adaptor:scratchbird-dbt-adapter` |
| driver\_package\_uuid | `019e12a0-0031-7000-8000-000000000031` |
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
| package\_type | `python_wheel` |
| license | MPL-2.0 |

---

## Installation

The artifact is a Python wheel, built with:

```bash
python3 -m build
```

Install smoke: `python3 -m pytest tests`.

No further install instructions are available from source at this time. The dbt adapter plugin mechanism (`AdapterPlugin`) and the `profiles.yml` connection profile format are standard dbt adapter conventions; specific profile key names for ScratchBird are not documented in the available source.

---

## API surface (from package_contract.json)

The contract declares the following API surface areas:

- `ConnectionManager` — manages connections to ScratchBird via the Python driver
- `AdapterPlugin` — dbt adapter plugin registration
- `relation` — dbt relation object (schema-qualified table/view references)
- `column` — dbt column type representation
- `materialization_table` — dbt table materialization
- `materialization_view` — dbt view materialization
- `incremental_strategy` — incremental model strategy

Schema metadata uses authorization-filtered `sys.information` views (not raw `information_schema`), consistent with the ScratchBird `sys_information_recursive` metadata profile.

---

## Authority boundaries

Per the `delegation_posture` in `package_contract.json`:

- The adapter may compile dbt relation and materialization requests
- The Python driver handles transport and protocol binding
- The server revalidates authentication, authorization, SBLR, UUID, cache, schema, and transaction claims
- MGA transaction finality remains engine-owned

---

## Type and feature mapping

Type mapping follows the `python_dbapi_mapping` profile. See [../type_mapping.md](../type_mapping.md).

---

## Capabilities and limitations

Source coverage is thin (package_contract.json only). The following is known from the contract:

- Supported materialization strategies: table, view, incremental
- Catalog metadata maps to ScratchBird `sys.information` with authorization-filtered visibility
- Comparison baseline is `dbt-postgres`; ScratchBird-specific deltas cover relation, column, materialization, incremental strategy, and adapter response surfaces

Runtime details, `profiles.yml` configuration keys, and incremental strategy semantics are not yet documented from source.

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
