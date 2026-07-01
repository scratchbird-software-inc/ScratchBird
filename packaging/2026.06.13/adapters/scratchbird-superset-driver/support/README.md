# ScratchBird Superset Driver

This package provides the ScratchBird SQLAlchemy dialect plus a Superset
DB engine spec. It enables Apache Superset to connect to ScratchBird using
SBWP v1.1 on the native port (3092).

## Install

From the repo root:

```bash
pip install -e scratchbird-superset-driver
```

Or install the published package once available:

```bash
pip install scratchbird-superset
```

## Superset Setup

1. Install this package into the Superset Python environment.
2. Restart Superset.
3. Add a new database using a SQLAlchemy URI like:

```
scratchbird://user:password@host:3092/database?sslmode=require
```

Notes:
- TLS-enabled modes are recommended; `sslmode=disable` remains available for
  explicit local-development/plaintext paths because the adapter rides on the
  Python driver parity surface.
- Default port is 3092.
- `binary_transfer=true` is recommended (binary-only protocol).
- JDBC-style query aliases such as `currentSchema`, `searchPath`,
  `applicationName`, `managerAuthToken`, and the full staged auth/bootstrap
  option family are normalized to the Python driver contract.
- If no explicit schema override is supplied, the dialect resolves the live
  session schema via `SHOW current_schema`, with `users.public` fallback.

## Development

- Entry points are registered for:
  - `sqlalchemy.dialects` (dialect name: `scratchbird`)
  - `superset.db_engine_specs` (engine spec: `ScratchBirdEngineSpec`)

The dialect delegates DB-API calls to the ScratchBird Python driver.

## References

- ScratchBird driver specs: `public_release_evidence`
- Superset integration spec: `docs/application-reference/SUPERSET_COMPATIBILITY_CONTRACT.md`
