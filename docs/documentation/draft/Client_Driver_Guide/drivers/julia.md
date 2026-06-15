# ScratchBird Julia Driver — DBInterface / Tables.jl Binding

> **Status: beta\_2 / release\_candidate (draft stub)** — The Julia driver
> source tree contains only a `package_contract.json` at the time of this
> writing. No `README.md`, `Project.toml`, source files, or implementation
> docs are present in the source tree. This page documents what is verifiable
> from the manifest and the package contract; all API examples below are drawn
> exclusively from those sources. The driver is not yet usable for application
> development. This page will be updated when source is committed.

## Purpose

The Julia driver is intended to provide ScratchBird connectivity for Julia
programs using the [DBInterface.jl](https://github.com/JuliaDatabases/DBInterface.jl)
standard and the [Tables.jl](https://github.com/JuliaData/Tables.jl) interface.
These two abstractions are the idiomatic Julia database API: `DBInterface.connect`,
`DBInterface.execute`, `DBInterface.prepare`, and the `Tables.rows` /
`Tables.columns` sinks.

Target audience: Julia data scientists and engineers who need ScratchBird
access from within the Julia ecosystem.

## Manifest Metadata

| Field                    | Value                                      |
|--------------------------|--------------------------------------------|
| `driver_package_uuid`    | `019e12a0-0027-7000-8000-000000000027`     |
| `driver_family`          | `julia`                                    |
| `api_surface_set`        | `dbinterface_tables`                       |
| `ingress_mode_set`       | `direct_listener`, `manager_proxy`         |
| `wire_protocol_set`      | `sbwp_v1_1`                                |
| `dsn_key_set`            | `database`, `host`, `port`, `user`, `auth_method` |
| `auth_method_set`        | `engine_local_password`, `scram_ready`     |
| `tls_profile_set`        | `scratchbird_tls_1_3_floor`                |
| `type_mapping_profile`   | `sbsql_core`                               |
| `diagnostic_mapping_profile` | `native_sqlstate`                      |
| `metadata_profile`       | `sys_information_recursive`                |
| `thread_safety_class`    | `thread_safe`                              |
| `pooling_capability`     | `connection_pool`                          |
| `release_bucket`         | `release_candidate`                        |
| `conformance_profile_ref`| `driver_julia_gate`                        |

## Public API Surface (from `package_contract.json`)

The following entry points are declared in the contract. They follow standard
DBInterface.jl conventions:

| Symbol                   | Role                                       |
|--------------------------|--------------------------------------------|
| `ScratchBird.Connection` | Connection type (implements `DBInterface.Connection`) |
| `DBInterface.connect`    | Open a connection                          |
| `DBInterface.execute`    | Execute a statement, return a cursor       |
| `DBInterface.prepare`    | Prepare a statement                        |
| `Tables.rows`            | Iterate cursor results as rows             |
| `Tables.columns`         | Access cursor results as column vectors    |

## Install

Not yet available. When published, the expected form is:

```julia
# Project.toml (anticipated; not yet verified)
[deps]
ScratchBird = "<uuid>"
```

```julia
using Pkg
Pkg.add("ScratchBird")
```

The package name is expected to be `ScratchBird` in the Julia registry
(unconfirmed; no `Project.toml` is present in the source tree).

## Connecting

The DSN form is expected to follow the standard ScratchBird URL convention:

```
scratchbird://user:password@host:port/database[?option=value&...]
```

Default port: 3092. See ../connection\_and\_dsn.md for the full key reference.

Anticipated usage based on DBInterface.jl conventions:

```julia
using ScratchBird, DBInterface, Tables

conn = DBInterface.connect(ScratchBird.Connection,
    "scratchbird://user:pass@localhost:3092/mydb")
```

> No source files are present to confirm the actual keyword arguments,
> connection options, or error types. The above is illustrative only.

## Executing Statements and Transactions

Anticipated pattern (DBInterface.jl standard):

```julia
# Query
result = DBInterface.execute(conn, "SELECT id, name FROM users")
for row in Tables.rows(result)
    println(row.id, row.name)
end

# Prepared statement
stmt = DBInterface.prepare(conn, "SELECT * FROM t WHERE id = ?")
result = DBInterface.execute(stmt, [42])
```

Transaction semantics conform to the MGA engine contract (source:
`package_contract.json` — `mga_transaction_finality`):

- sessions are always in a transaction
- `COMMIT` / `ROLLBACK` reopen the next boundary
- no hidden replay of abandoned in-flight transactions
- `mga_transaction_finality` route requirement is declared in the contract

## Conformance (Declared in Contract)

The following conformance areas are declared in `package_contract.json`:

| Area                 | Declared |
|----------------------|----------|
| `connect_auth`       | yes      |
| `prepare_execute_fetch` | yes   |
| `transactions`       | yes      |
| `metadata`           | yes      |
| `type_mapping`       | yes      |
| `error_mapping`      | yes      |
| `reconnect`          | yes      |
| `protocol_negotiation` | yes    |
| `cancellation`       | yes      |

> These are contract declarations, not verified against implemented source.
> See [../conformance\_baseline.md](../conformance_baseline.md) for the
> `driver_julia_gate` conformance profile once source is available.

## What Is Not Yet Covered

Because the source tree contains only `package_contract.json`, the following
sections cannot be documented from source and are omitted:

- Actual `Project.toml` package name, version, and dependencies
- Concrete connection constructor keyword arguments
- Error type hierarchy and exception names
- Type mapping table (Julia types for each SBsql OID)
- Metadata query wrapper methods
- Pooling configuration parameters
- TLS and authentication option details beyond DSN key names

These sections will be added when source is committed to the driver directory.

## See Also

- [../README.md](../README.md) — Client & Driver Guide overview
- [../connection\_and\_dsn.md](../connection_and_dsn.md)
- [../authentication.md](../authentication.md)
- [../wire\_protocol\_sbwp.md](../wire_protocol_sbwp.md)
- [../type\_mapping.md](../type_mapping.md)
- [../metadata\_sys\_information.md](../metadata_sys_information.md)
- [../diagnostics\_and\_sqlstate.md](../diagnostics_and_sqlstate.md)
- [../pooling\_and\_concurrency.md](../pooling_and_concurrency.md)
- [../conformance\_baseline.md](../conformance_baseline.md)
