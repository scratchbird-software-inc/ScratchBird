# Documentation And Sample Application Gate

Search key: `DRIVER_SERVER_DOCUMENTATION_SAMPLE_APP`.

## Purpose

Ensure documented driver and adapter behavior is executable release evidence.

## Required Sample Coverage

- DSN and connection-string examples.
- Authentication and TLS setup.
- Transaction begin, commit, rollback, savepoint, and autocommit behavior.
- Prepared statement parameter binding.
- Result metadata and type round-trip.
- Diagnostics and SQLSTATE handling.
- Cancel and timeout handling.
- Pool reset or adapter lifecycle behavior where applicable.
- DBeaver, ODBC, JDBC, .NET, Python, CLI, and other claimed lane entry points.

## Closure Rule

`DSR-044` closes only when documentation samples run in CTest or a cited lane
smoke harness and use the declared ScratchBird route.
