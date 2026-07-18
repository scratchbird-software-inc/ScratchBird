# SBSQL Public Per-Element Contract Snapshots

This generator-owned directory contains one deterministic Markdown snapshot for each published SBSQL surface row.

## Inputs

- `SURFACE_IMPLEMENTATION_BACKLOG.csv`
- `SEMANTIC_ORACLE_AUTHORITY_MAP.csv`
- `SBSQL_SURFACE_RELEASE_DECLARATION.csv`

All inputs are tracked public release artifacts. The generator reads them without mutation and does not require network access.

## Layout

Each `SBSQL-<12 uppercase hexadecimal digits>.md` file contains one surface identity, route, closure, and oracle snapshot. There is no shared per-surface output file.

Published surface snapshots: 2617

## Boundaries

These files are generated public evidence. They do not execute SQL, do not grant parser or engine authority, and contain no private canonicalization or source-tree path references.
