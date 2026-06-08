# Storage Database Runtime Foundation

This package implements the first database create/open lifecycle skeleton for the engine-first runtime path.

## Scope

- create a database file through the storage disk abstraction;
- write and read the database header;
- enforce UUIDv7 database identity;
- write initial managed page headers for database-header, allocation, catalog, and transaction-inventory pages;
- preserve safe recognition of cluster/unknown structures without activating cluster authority.

## Non-scope

This slice does not implement final page bodies, final catalog pages, encryption, cluster authority, recovery, archival, or WAL. MGA/copy-on-write remains the internal journaling model.
