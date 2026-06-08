# PostgreSQL 18.1 exact rowset extraction status

Search key: `SB_REFERENCE_POSTGRESQL_18_1_EXACT_ROWSET_EXTRACTION_STATUS`

Status: incomplete, extraction required before implementation-ready emulation setup.

Required exact seed rowsets:

1. Catalog object definitions for every `pg_catalog` table and view required by the expansion pack.
2. Default namespace rows such as `pg_catalog`, `information_schema`, and `public` when created by the selected recipe.
3. Built-in type rows and array/range/multirange companion rows.
4. Built-in functions/procedures in `pg_proc`.
5. Built-in operators, operator classes, operator families, access methods, casts, aggregates, collations, conversions, text-search objects, and languages.
6. Initial database, tablespace, role, privilege, dependency, description, and extension rows.
7. Publication/subscription/logical replication catalog rows for an empty newly-created profile.
8. Generated OID, relfilenode, toast, ACL, and owner values represented by deterministic UUID-backed emulation rules.

Rows containing instance-specific paths, database OIDs, role passwords, ACL secrets, file nodes, timestamps, or cluster identifiers must be redacted/generated and never copied as ScratchBird UUID identity.
