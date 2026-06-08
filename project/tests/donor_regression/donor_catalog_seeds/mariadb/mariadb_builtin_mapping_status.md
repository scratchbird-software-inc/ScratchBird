# MariaDB Built-in Mapping Status

Search key: SB_REFERENCE_DONOR_MARIADB_BUILTIN_MAPPING_STATUS

The MariaDB built-in inventory is a first-pass behavior inventory. It is not yet a complete signature table. The MariaDB donor profile must not be marked implementation-ready until every built-in and SQL-mode-sensitive construct has an exact mapping row.

## Required mapping classes

| Class | Authority |
|---|---|
| Native common scalar functions | Native v3 AST and SBLR operators. |
| SQL-mode-sensitive functions | MariaDB parser plus C++ UDR where behavior differs from native ScratchBird. |
| Dynamic-column functions | MariaDB C++ UDR compatibility package unless native structured-domain support is explicitly equivalent. |
| Encryption and password functions | ScratchBird security policy plus C++ UDR; raw secrets forbidden. |
| UUID functions | ScratchBird UUID generator policy; UUIDs are never reused; UUID v7 obeys cluster time rules. |
| INET functions | ScratchBird INET4/INET6 domains and exact cast/comparison/index rules. |
| JSON and MYSQL_JSON behavior | Native JSON domain plus MySQL/MariaDB binary JSON restore bridge where required. |
| XMLTYPE behavior | XML domain or MariaDB C++ UDR; unsupported XMLTYPE attributes fail with diagnostics. |
| Metadata/session functions | Donor projection over ScratchBird session, transaction, security, catalog, and metrics state. |

## Required exact mapping row fields

Each final row must define:

- donor name
- donor grammar context
- SQL mode sensitivity
- argument signature
- implicit cast rules
- return signature
- charset/collation behavior
- null behavior
- determinism class
- native v3 AST production
- SBLR mapping or C++ UDR entry point
- session, transaction, security, and metrics dependencies
- warnings and diagnostic vectors
- conformance tests

## Diagnostics

| Condition | Diagnostic vector |
|---|---|
| Exact signature missing | DONOR.MARIADB.BUILTIN.SIGNATURE_MISSING |
| SQL mode behavior unspecified | DONOR.MARIADB.BUILTIN.SQL_MODE_UNSPECIFIED |
| C++ UDR entry point missing | DONOR.MARIADB.BUILTIN.UDR_ENTRY_MISSING |
| Raw secret exposure attempted | DONOR.MARIADB.BUILTIN.RAW_SECRET_FORBIDDEN |
| UUID v7 called without required cluster-time authority | DONOR.MARIADB.UUID.CLUSTER_TIME_REQUIRED |
| Unsupported plugin datatype behavior | DONOR.MARIADB.TYPE.UNSUPPORTED |
