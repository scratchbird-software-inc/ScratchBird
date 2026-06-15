# Conformance Baseline

## Purpose

This page describes the staged S1-S5 conformance baseline that all ScratchBird drivers are
validated against, the BASELINE_REQUIREMENT_MAPPING.md artifact each driver maintains, the
conformance gates referenced in the manifest, and what `beta_2` / `release_candidate` status
means for production use.

Sources used: `project/drivers/driver/python/BASELINE_REQUIREMENT_MAPPING.md`,
`project/drivers/DriverPackageManifest.csv`, `project/drivers/tool/cli/package_contract.json`.

This is a **draft**. Components are in `beta_2` / `release_candidate` status.

---

## Release Status

The manifest records two status values for each component:

| Column | Value | Meaning |
| --- | --- | --- |
| `driver_status` | `beta_2` | The driver has passed second-beta validation. API surface is stabilizing but not yet frozen. |
| `release_bucket` | `release_candidate` | The component is in release-candidate state — it has cleared beta gates and is under final release testing. |

These designations apply to all 21 drivers, all 12 adaptors, and the CLI tool.

Source: `DriverPackageManifest.csv` columns `driver_status`, `release_bucket`.

**No component in this guide should be assumed production-certified** until the release gate
listed in its `conformance_profile_ref` has been formally closed. Live server verification and
release evidence are outstanding closure steps for most drivers.

---

## The S1–S5 Staged Baseline

Conformance is organized into five stages. Each stage builds on the previous.

| Stage | Short Name | Requirement Group |
| --- | --- | --- |
| S1 | CONN | Connection: DSN parsing, ingress mode, TLS, auth negotiation, session opening, session schema, liveness |
| S2 | TXN / EXEC | Transaction and execution: begin, commit, rollback, savepoints, prepared transactions, isolation levels, simple and extended query, batch, multi-result, callable, generated keys |
| S3 | META | Metadata: collection routing, restriction filtering, recursive schema navigation, DDL-editor payload |
| S4 | ERR / RES | Error and resource: error hierarchy, SQLSTATE mapping, retry boundaries, pool lifecycle, circuit breaker, keepalive, leak detection |
| S5 | TYPE | Type mapping: scalar, temporal, JSON, range, composite, vector, array, LOB wrappers, unknown-type fallback |

The naming convention `S1` through `S5` maps to the conformance profile reference in the
manifest (e.g., `driver_python_gate` references the Python driver's full stage completion).

Source: `project/drivers/driver/python/BASELINE_REQUIREMENT_MAPPING.md` — "Status legend" and
table of PYTHONBL groups mapped to JDBC baseline groups.

---

## BASELINE_REQUIREMENT_MAPPING.md

Each driver maintains a `BASELINE_REQUIREMENT_MAPPING.md` file that records:

- The mapping of driver-specific groups (e.g., `PYTHONBL-CONN`) to the shared JDBC baseline
  groups (e.g., `JDBCBL-CONN`).
- The current implementation status of each group: `Implemented`, `Partial`, or `Missing`.
- The source and test anchors that provide evidence for each implemented group.
- Notes on known scope limits or outstanding steps.

The Python driver's BASELINE_REQUIREMENT_MAPPING records the `2026-04-03` JDBC/.NET-class
baseline closure date. A broader shared auth/bootstrap ratchet was introduced on `2026-04-17`.

Source: `project/drivers/driver/python/BASELINE_REQUIREMENT_MAPPING.md`.

---

## Python Driver Baseline Status (as of source audit)

| Stage | JDBC Baseline Group | Status | Outstanding |
| --- | --- | --- | --- |
| S1 CONN | JDBCBL-CONN | Implemented | Live server verification and release evidence |
| S2 TXN | JDBCBL-TXN | Implemented | Live server verification and release evidence |
| S2 EXEC | JDBCBL-EXEC | Implemented | Live server verification and release evidence |
| S3 META | JDBCBL-META | Implemented | Live server verification and release evidence |
| S5 TYPE | JDBCBL-TYPE | Implemented | Live server verification and release evidence |
| S4 ERR | JDBCBL-ERR | Implemented | Live server verification and release evidence |
| S4 RES | JDBCBL-RES | Implemented | Live server verification and release evidence |

Source: `project/drivers/driver/python/BASELINE_REQUIREMENT_MAPPING.md` — table rows.

**Note:** The Python driver's lane note states: "live server verification and release evidence
remain the outstanding closure step." Status `Implemented` means the behavior is present and
anchored by lane source and tests; it does not certify server-validated production readiness.

---

## MGA Recovery Contract

All drivers that implement the S2 (TXN/EXEC) stage follow ScratchBird's MGA (Multi-Generational
Architecture) recovery model:

- Reconnect or reopen repairs transport and session state only.
- Reconnect never resurrects abandoned in-flight transactions or replays lost statements.
- Transaction recovery means: reset, rollback, reopen, or retry against engine truth.
- Result resume is valid only for explicit suspended protocol states (after `PORTAL_SUSPENDED`).
- ScratchBird sessions are always in a transaction; `COMMIT` / `ROLLBACK` immediately reopen
  the next transaction boundary.
- Autocommit mode transitions are local driver policy on native lanes; the driver does not
  push a synthetic `SET_OPTION autocommit` wire message.
- `READ UNCOMMITTED` is a legacy compatibility alias; `REPEATABLE READ` maps to canonical
  `SNAPSHOT`; `SERIALIZABLE` maps to canonical `SNAPSHOT TABLE STABILITY`.

Source: `project/drivers/driver/python/BASELINE_REQUIREMENT_MAPPING.md` — "MGA Recovery Contract".

---

## Conformance Gates

The manifest `conformance_profile_ref` column names the gate that must be closed before a
component is considered release-ready. The known gates are:

| Component | Gate |
| --- | --- |
| adbc | driver_adbc_gate |
| flightsql | driver_flightsql_gate |
| julia | driver_julia_gate |
| perl | driver_perl_gate |
| r2dbc | driver_r2dbc_gate |
| cpp | driver_cpp_gate |
| dart | driver_dart_gate |
| dotnet | driver_dotnet_gate |
| elixir | driver_elixir_gate |
| go | driver_go_gate |
| jdbc | driver_jdbc_gate |
| mojo | driver_mojo_gate |
| node | driver_node_gate |
| odbc | driver_odbc_gate |
| pascal | driver_pascal_gate |
| php | driver_php_gate |
| python | driver_python_gate |
| r | driver_r_gate |
| ruby | driver_ruby_gate |
| rust | driver_rust_gate |
| swift | driver_swift_gate |
| scratchbird-airbyte | adaptor_airbyte_gate |
| scratchbird-dbt-adapter | adaptor_dbt_gate |
| scratchbird-looker | adaptor_looker_gate |
| scratchbird-powerbi | adaptor_powerbi_gate |
| scratchbird-tableau | adaptor_tableau_gate |
| scratchbird-dbeaver-driver | adaptor_dbeaver_gate |
| scratchbird-hibernate-dialect | adaptor_hibernate_gate |
| scratchbird-metabase-driver | adaptor_metabase_gate |
| scratchbird-prisma-adapter | adaptor_prisma_gate |
| scratchbird-sqlalchemy-dialect | adaptor_sqlalchemy_gate |
| scratchbird-superset-driver | adaptor_superset_gate |
| scratchbird-typeorm-adapter | adaptor_typeorm_gate |
| CLI tool | tool_cli_gate |

Source: `DriverPackageManifest.csv` column `conformance_profile_ref`.

---

## CLI Conformance Tool

The CLI package includes `sbdriver_conformance` — a standalone conformance manifest runner.

Key points:
- A conformance manifest (`conformance/sbwp_conformance_manifest.sample.json`) describes
  test cases with typed assertions including `expect_columns`, `expect_column_type_oids`,
  `expect_first_row_json`, `expect_first_row_types`, and `expect_rows_json`.
- The runner (`conformance/run_sbdriver_conformance_sample.sh`) accepts `--binary-params`,
  `--text-params`, `--manifest`, `--output`, and `--no-build` flags.
- The runner requires `SB_CONFORMANCE_DSN` to be set to a live endpoint.

Source: `project/drivers/tool/cli/README.md` — "Conformance Sample".

---

## Building and Running Driver Gates

Gates are run via CTest from the project build system:

```bash
cmake -S project -B build/driver_gates -DSB_BUILD_DRIVER_GATES=ON
ctest --test-dir build/driver_gates -L driver --output-on-failure
```

Source: `project/drivers/README.md` — "CTest Gates".

---

## Cross-References

- [connection_and_dsn.md](connection_and_dsn.md) — S1 CONN detail
- [authentication.md](authentication.md) — S1 CONN auth negotiation
- [type_mapping.md](type_mapping.md) — S5 TYPE detail
- [metadata_sys_information.md](metadata_sys_information.md) — S3 META detail
- [diagnostics_and_sqlstate.md](diagnostics_and_sqlstate.md) — S4 ERR detail
- [pooling_and_concurrency.md](pooling_and_concurrency.md) — S4 RES detail
