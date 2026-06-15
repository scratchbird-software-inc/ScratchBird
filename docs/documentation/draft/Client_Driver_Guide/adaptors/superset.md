# Apache Superset Adaptor

> **beta_2 / release_candidate.**
> This page describes the ScratchBird Superset driver package, which provides
> a SQLAlchemy dialect and an Apache Superset DB engine spec.

## Purpose

The `scratchbird-superset` package integrates ScratchBird — a Convergent Data
Engine (CDE) speaking SBWP v1.1 on default port 3092 — into Apache Superset.
It provides two entry points that Superset discovers at startup:

- A **SQLAlchemy dialect** (`ScratchBirdDialect`) registered under the
  `scratchbird` scheme, which translates SQLAlchemy operations to ScratchBird
  DB-API calls.
- A **Superset DB engine spec** (`ScratchBirdEngineSpec`) that teaches Superset
  ScratchBird-specific behaviour: time-grain expressions, schema metadata
  discovery, limit method, catalog support, and DirectQuery configuration.

Both components delegate all DB-API I/O to the ScratchBird Python driver
(`driver:python`).  The adaptor does not speak the SBWP wire itself; it shapes
Superset EngineSpec and SQLAlchemy requests and lets the Python driver handle
transport, authentication, and protocol binding.

**Release status:** beta_2, release_candidate.

---

## Manifest Metadata

| Field | Value |
|---|---|
| `component_id` | `adaptor:scratchbird-superset-driver` |
| `driver_package_uuid` | `019e12a0-0022-7000-8000-000000000022` |
| `api_surface_set` | `application_adapter` |
| `ingress_mode_set` | `driver_embedded_python` |
| `wire_protocol_set` | `sbwp_v1_1` |
| `dsn_key_set` | `database`, `host`, `port`, `user`, `auth_method` |
| `auth_method_set` | `engine_local_password`, `scram_ready` |
| `tls_profile_set` | `scratchbird_tls_1_3_floor` |
| `type_mapping_profile` | `python_dbapi_mapping` |
| `diagnostic_mapping_profile` | `native_sqlstate` |
| `metadata_profile` | `sys_information_recursive` |
| `thread_safety_class` | `connection_thread_confined` |
| `pooling_capability` | `delegates_to_python` |
| `release_bucket` | `release_candidate` |
| `conformance_profile_ref` | `adaptor_superset_gate` |
| Delegates to | `driver:python` (ScratchBird Python driver) |
| Package type | `python_wheel` |
| Published package name | `scratchbird-superset` |
| License | Apache-2.0 |

**Delegation posture:** `delegates_to_python`.  The adaptor shapes Superset
EngineSpec and SQLAlchemy requests; the Python driver handles transport and
protocol binding.  Authentication authority, MGA transaction finality,
authorization, and UUID identity remain engine-owned.

---

## API Surfaces

| Surface | Implementation class / entry point |
|---|---|
| SQLAlchemy dialect | `scratchbird_superset.dialect.ScratchBirdDialect` |
| Superset DB engine spec | `scratchbird_superset.engine_spec.ScratchBirdEngineSpec` |
| Time-grain expressions | `ScratchBirdEngineSpec._time_grain_expressions` (second through year, via `DATE_TRUNC`) |
| Schema metadata | `supports_dynamic_schema = True`, `supports_catalog = True` |
| Limit method | `LimitMethod.FORCE_LIMIT` |

Entry points registered in `pyproject.toml`:

```
sqlalchemy.dialects: scratchbird = scratchbird_superset.dialect:ScratchBirdDialect
superset.db_engine_specs: scratchbird = scratchbird_superset.engine_spec:ScratchBirdEngineSpec
```

---

## Installation

Install the wheel into the Superset Python environment (the same environment
that runs Superset workers and the web server):

```bash
# From the repo (development / pre-release)
pip install -e project/drivers/adaptor/scratchbird-superset-driver

# Once published
pip install scratchbird-superset
```

This also installs `scratchbird>=0.1.0` (the Python driver) as a dependency.

After installing, **restart Superset** (web server and all Celery workers) so
that the new entry points are picked up.

Build the wheel for distribution:

```bash
cd project/drivers/adaptor/scratchbird-superset-driver
python3 -m build
# Output: dist/scratchbird_superset-0.1.0-py3-none-any.whl
```

---

## Configuring a Connection in Superset

In Superset, go to **Settings → Database Connections → + Database** and either:

- Select **ScratchBird** from the supported database list (if the EngineSpec
  registers the connector in the UI), or
- Choose **Other** and enter a SQLAlchemy URI manually.

The SQLAlchemy URI format:

```
scratchbird://user:password@host:3092/database?sslmode=require
```

| URI component | Notes |
|---|---|
| `user` | ScratchBird user principal |
| `password` | Password (`engine_local_password` / `scram_ready`) |
| `host` | Hostname or IP address |
| `port` | Default: 3092 |
| `database` | Database name |
| `sslmode` | Recommended: `require`.  `disable` is available for local development only. |

**Additional connection-string options** recognized by the dialect:

| Option | Notes |
|---|---|
| `binary_transfer=true` | Recommended; enables binary-only protocol transfer |
| `currentSchema` / `searchPath` | Schema override; if omitted, the session schema resolves via `SHOW current_schema` with `users.public` fallback |
| `applicationName` | Application tag visible in server-side session views |
| `managerAuthToken` | Manager-proxy authentication token (if using `manager_proxy` ingress) |

The dialect normalizes JDBC-style aliases (`currentSchema`, `searchPath`,
`applicationName`, `managerAuthToken`, and the full staged auth/bootstrap
option family) to the Python driver contract.  See
[../drivers/python.md](../drivers/python.md) for the full Python driver
parameter reference.

TLS is always required in production (`scratchbird_tls_1_3_floor`).  See
[../tls_profiles.md](../tls_profiles.md).

Authentication is engine-owned.  See [../authentication.md](../authentication.md).

---

## Type Mapping

This adaptor uses the `python_dbapi_mapping` type-mapping profile.
See [../type_mapping.md](../type_mapping.md) for the full mapping table.

---

## Capabilities and Limitations

| Capability | Status |
|---|---|
| Charts and dashboards | Supported (standard Superset SQL execution) |
| Dynamic schema / catalog | Supported (`supports_dynamic_schema`, `supports_catalog`) |
| Joins in SQL Lab | Supported (`allows_joins = True`) |
| Subqueries | Supported (`allows_subqueries = True`) |
| Time-grain aggregations | Supported (second, minute, hour, day, week, month, quarter, year) |
| Authentication | Engine-owned via Python driver (password, SCRAM-ready) |
| MGA transaction finality | Engine-owned; adaptor cannot override |
| Authorization | Engine-owned; invalid auth/bootstrap values fail closed |
| UUID catalog identity | Engine-owned |
| Transport / protocol | Delegated to ScratchBird Python driver |
| TLS | `scratchbird_tls_1_3_floor`; `sslmode=disable` available for local dev only |

---

## Diagnostics

SQLSTATE codes use the `native_sqlstate` diagnostic mapping profile, surfaced
through the Python driver.
See [../diagnostics_and_sqlstate.md](../diagnostics_and_sqlstate.md).

Test suite: `python3 -m pytest -q tests` (includes `test_superset_contract.py`
and `test_package_contract.py`).

---

## See Also

- [../README.md](../README.md) — Client and Driver Guide overview
- [../drivers/python.md](../drivers/python.md) — ScratchBird Python driver (underlying driver)
- [../connection_and_dsn.md](../connection_and_dsn.md) — DSN and connection-string reference
- [../authentication.md](../authentication.md) — Authentication methods
- [../tls_profiles.md](../tls_profiles.md) — TLS profiles
- [../type_mapping.md](../type_mapping.md) — Type mapping profiles
- [../diagnostics_and_sqlstate.md](../diagnostics_and_sqlstate.md) — Diagnostics and SQLSTATE
