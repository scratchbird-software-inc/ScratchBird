# ScratchBird Client and Driver Guide

## Purpose

This guide is the reference for every component that connects an application to a ScratchBird
Convergent Data Engine (CDE). It covers the shared connection model, wire protocol, authentication,
TLS, type mapping, diagnostics, pooling, metadata, conformance, and CLI tools — the backbone that
all drivers and adaptors build on. Per-driver and per-adaptor detail is in the individual chapters
listed in the tables below.

This is a **draft**. All components documented here carry a `beta_2` driver status and
`release_candidate` release bucket. No claim in this guide constitutes a production certification
or a promise of binary stability beyond that designation. Consult the per-component
conformance gate listed in the manifest before deploying any component to a production workload.

## API Boundary Rule

Drivers, adaptors, and tools must communicate through supported public server, wire, parser, or
driver APIs for their category. They must not link directly to private engine internals.

Source: `project/drivers/README.md` — "Drivers, adaptors, and tools must not link directly to
private engine internals. They must communicate through the supported public server, wire, parser,
or driver APIs for their category."

---

## Shared Chapters

| Chapter | Contents |
| --- | --- |
| [connection_and_dsn.md](connection_and_dsn.md) | Connection model, ingress modes, DSN key set, session opening, auth bootstrap flow |
| [authentication.md](authentication.md) | Client-side credential supply, auth_method negotiation, engine_local_password and scram_ready |
| [tls_profiles.md](tls_profiles.md) | The scratchbird_tls_1_3_floor TLS profile and what clients must present |
| [wire_protocol_sbwp.md](wire_protocol_sbwp.md) | ScratchBird Wire Protocol (SBWP) sbwp_v1_1: role, negotiation, framing, relationship to SBPS |
| [type_mapping.md](type_mapping.md) | The sbsql_core type-mapping profile: canonical type to client-representation table |
| [diagnostics_and_sqlstate.md](diagnostics_and_sqlstate.md) | native_sqlstate profile: how message vectors and refusal vectors surface to clients |
| [pooling_and_concurrency.md](pooling_and_concurrency.md) | thread_safety_class and pooling_capability values and what they mean for client developers |
| [metadata_sys_information.md](metadata_sys_information.md) | sys_information_recursive: introspecting schemas and objects via sys.information.* projections |
| [conformance_baseline.md](conformance_baseline.md) | S1-S5 staged baseline, BASELINE_REQUIREMENT_MAPPING, conformance gates, release status |
| [cli_tools.md](cli_tools.md) | CLI utilities: sb_isql, sb_admin, sb_backup, sb_security, sb_verify, compatibility front-ends |

---

## Drivers

All 21 drivers share the following manifest values unless noted in the per-driver page:
ingress modes `direct_listener` and `manager_proxy`, wire protocol `sbwp_v1_1`,
auth methods `engine_local_password` and `scram_ready`, TLS profile `scratchbird_tls_1_3_floor`,
metadata profile `sys_information_recursive`, diagnostic profile `native_sqlstate`,
driver status `beta_2`, release bucket `release_candidate`.

Source: `project/drivers/DriverPackageManifest.csv`.

| Family | Name | API Surface | Type Mapping | Thread Safety | Pooling | Conformance Gate | Driver Page |
| --- | --- | --- | --- | --- | --- | --- | --- |
| adbc | ADBC | adbc_c_api | arrow_recordbatch | thread_safe | connection_pool | driver_adbc_gate | [drivers/adbc.md](drivers/adbc.md) |
| flight_sql | FlightSQL | flight_sql_grpc | arrow_recordbatch | thread_safe | stream_pool | driver_flightsql_gate | [drivers/flightsql.md](drivers/flightsql.md) |
| julia | Julia | dbinterface_tables | sbsql_core | thread_safe | connection_pool | driver_julia_gate | [drivers/julia.md](drivers/julia.md) |
| perl | Perl | perl_dbi | sbsql_core | connection_thread_confined | connection_pool | driver_perl_gate | [drivers/perl.md](drivers/perl.md) |
| r2dbc | R2DBC | r2dbc_spi | sbsql_core | thread_safe | reactive_pool | driver_r2dbc_gate | [drivers/r2dbc.md](drivers/r2dbc.md) |
| c_cpp | C/C++ | native_cli_c_api | sbsql_core | thread_safe | session_pool; statement_cache | driver_cpp_gate | [drivers/cpp.md](drivers/cpp.md) |
| dart | Dart | language_binding | sbsql_core | thread_safe | session_pool | driver_dart_gate | [drivers/dart.md](drivers/dart.md) |
| dotnet | .NET | ado_net | sbsql_core | thread_safe | connection_pool | driver_dotnet_gate | [drivers/dotnet.md](drivers/dotnet.md) |
| elixir | Elixir | language_binding | sbsql_core | thread_safe | session_pool | driver_elixir_gate | [drivers/elixir.md](drivers/elixir.md) |
| go | Go | database_sql | sbsql_core | thread_safe | connection_pool | driver_go_gate | [drivers/go.md](drivers/go.md) |
| jdbc | JDBC | jdbc_4_x | sbsql_core | thread_safe | connection_pool | driver_jdbc_gate | [drivers/jdbc.md](drivers/jdbc.md) |
| mojo | Mojo | language_binding | sbsql_core | thread_safe | session_pool | driver_mojo_gate | [drivers/mojo.md](drivers/mojo.md) |
| node | Node.js | language_binding | sbsql_core | thread_safe | connection_pool | driver_node_gate | [drivers/node.md](drivers/node.md) |
| odbc | ODBC | odbc_3_x | sbsql_core | thread_safe | connection_pool | driver_odbc_gate | [drivers/odbc.md](drivers/odbc.md) |
| pascal | Pascal | language_binding | sbsql_core | thread_safe | session_pool | driver_pascal_gate | [drivers/pascal.md](drivers/pascal.md) |
| php | PHP | language_binding | sbsql_core | connection_thread_confined | session_pool | driver_php_gate | [drivers/php.md](drivers/php.md) |
| python | Python | dbapi_2 | sbsql_core | thread_safe | connection_pool | driver_python_gate | [drivers/python.md](drivers/python.md) |
| r | R | dbi | sbsql_core | connection_thread_confined | session_pool | driver_r_gate | [drivers/r.md](drivers/r.md) |
| ruby | Ruby | language_binding | sbsql_core | thread_safe | connection_pool | driver_ruby_gate | [drivers/ruby.md](drivers/ruby.md) |
| rust | Rust | language_binding | sbsql_core | thread_safe | connection_pool | driver_rust_gate | [drivers/rust.md](drivers/rust.md) |
| swift | Swift | language_binding | sbsql_core | thread_safe | session_pool | driver_swift_gate | [drivers/swift.md](drivers/swift.md) |

**Notes:**
- The ADBC driver uses `arrow_recordbatch` type mapping, not `sbsql_core`. See [drivers/adbc.md](drivers/adbc.md).
- The FlightSQL driver uses `arrow_recordbatch` type mapping and a `grpc_status_sqlstate` diagnostic profile rather than `native_sqlstate`. Its DSN key set adds `flight_endpoint`. See [drivers/flightsql.md](drivers/flightsql.md).
- The ODBC driver adds a `dsn` key to the standard DSN key set. See [drivers/odbc.md](drivers/odbc.md).

---

## Adaptors

All 12 adaptors carry `beta_2` driver status, `release_candidate` release bucket, `sbwp_v1_1`
wire protocol, and `native_sqlstate` diagnostic profile unless noted.

| Name | API Surface | Ingress Mode(s) | Type Mapping | Pooling | Conformance Gate | Adaptor Page |
| --- | --- | --- | --- | --- | --- | --- |
| scratchbird-airbyte | application_adapter | direct_listener; manager_proxy | python_dbapi_mapping | delegates_to_python | adaptor_airbyte_gate | [adaptors/airbyte.md](adaptors/airbyte.md) |
| scratchbird-dbt-adapter | application_adapter | direct_listener; manager_proxy | python_dbapi_mapping | delegates_to_python | adaptor_dbt_gate | [adaptors/dbt.md](adaptors/dbt.md) |
| scratchbird-looker | application_adapter | driver_embedded_jdbc | jdbc_mapping | delegates_to_jdbc | adaptor_looker_gate | [adaptors/looker.md](adaptors/looker.md) |
| scratchbird-powerbi | application_adapter | direct_listener; manager_proxy | powerquery_mapping | explicit_session | adaptor_powerbi_gate | [adaptors/powerbi.md](adaptors/powerbi.md) |
| scratchbird-tableau | application_adapter | direct_listener; manager_proxy | tableau_mapping | explicit_session | adaptor_tableau_gate | [adaptors/tableau.md](adaptors/tableau.md) |
| scratchbird-dbeaver-driver | application_adapter | manager_proxy; driver_embedded_jdbc | jdbc_mapping | delegates_to_jdbc | adaptor_dbeaver_gate | [adaptors/dbeaver.md](adaptors/dbeaver.md) |
| scratchbird-hibernate-dialect | application_adapter | driver_embedded_jdbc | jdbc_mapping | delegates_to_jdbc | adaptor_hibernate_gate | [adaptors/hibernate.md](adaptors/hibernate.md) |
| scratchbird-metabase-driver | application_adapter | driver_embedded_jdbc | jdbc_mapping | delegates_to_jdbc | adaptor_metabase_gate | [adaptors/metabase.md](adaptors/metabase.md) |
| scratchbird-prisma-adapter | application_adapter | driver_embedded_node | sbsql_core | delegates_to_node | adaptor_prisma_gate | [adaptors/prisma.md](adaptors/prisma.md) |
| scratchbird-sqlalchemy-dialect | application_adapter | driver_embedded_python | python_dbapi_mapping | delegates_to_python | adaptor_sqlalchemy_gate | [adaptors/sqlalchemy.md](adaptors/sqlalchemy.md) |
| scratchbird-superset-driver | application_adapter | driver_embedded_python | python_dbapi_mapping | delegates_to_python | adaptor_superset_gate | [adaptors/superset.md](adaptors/superset.md) |
| scratchbird-typeorm-adapter | application_adapter | driver_embedded_node | sbsql_core | delegates_to_node | adaptor_typeorm_gate | [adaptors/typeorm.md](adaptors/typeorm.md) |

**Notes:**
- Looker, DBeaver, Hibernate, and Metabase use `driver_embedded_jdbc` ingress and `jdbc_mapping` type mapping.
- Looker, Hibernate, and Metabase use `jdbc_url` as their primary DSN key instead of the standard `database;host;port` triple.
- DBeaver uses `manager_proxy;driver_embedded_jdbc` (both modes listed).
- Prisma and TypeORM use `driver_embedded_node` ingress and `sbsql_core` type mapping.

---

## CLI Tools

| Tool | Purpose | Page |
| --- | --- | --- |
| sb_isql | Interactive SBsql shell (SBsql dialect, Firebird ISQL compatible with PostgreSQL psql extensions) | [cli_tools.md](cli_tools.md) |
| sb_admin | Scheduler administration and metrics access | [cli_tools.md](cli_tools.md) |
| sb_backup | Backup, restore, and backup-file verification | [cli_tools.md](cli_tools.md) |
| sb_security | User, role, permission, and audit administration | [cli_tools.md](cli_tools.md) |
| sb_verify | Database integrity and consistency verification | [cli_tools.md](cli_tools.md) |
| sb_fb_isql | Firebird SQL syntax compatibility front-end | [cli_tools.md](cli_tools.md) |
| sb_my_isql | MySQL wire protocol compatibility front-end | [cli_tools.md](cli_tools.md) |
| sb_pg_isql | PostgreSQL wire protocol compatibility front-end | [cli_tools.md](cli_tools.md) |

---

## Cross-References

- [Operations and Administration Guide](../Operations_Administration/README.md) — operator procedures for backup, restore, diagnostics, and service lifecycle
- [Security Guide](../Security_Guide/README.md) — engine-side authentication providers, authorization, and cryptographic policy
- [Language Reference — Data Types](../Language_Reference/data_types/index.md) — canonical SBsql type names
- [Language Reference — Refusal Vectors](../Language_Reference/syntax_reference/refusal_vectors.md) — how the engine classifies refused requests
- [Language Support](../Language_Support/README.md) — localized/multilingual SBsql. A driver's language-surface capabilities (completion, canonical preview, localized diagnostics, the editor tool protocol) are defined in the language surface manifest and explained in the Language Support manual; this guide does not restate them. See [Client And Editor Language Surface](../Language_Support/client_and_editor_language_surface.md).
