# MySQL 8.4 exact rowset extraction status

Search key: `SB_REFERENCE_MYSQL_8_4_EXACT_ROWSET_EXTRACTION_STATUS`

Status: incomplete, extraction required before implementation-ready emulation setup.

Required exact seed rowsets:

1. MySQL data dictionary table definitions, columns, indexes, generated values, and initial rows.
2. `character_sets` and `collations` built-in rowsets.
3. `st_spatial_reference_systems` built-in rowsets.
4. `dd_properties` rows and generated property emulation rules.
5. System schema rows for `mysql`, `information_schema`, `performance_schema`, and `sys` profile surfaces.
6. Default users/roles/grants with authentication secrets redacted.
7. Plugin/component/resource group defaults.
8. Default time zone/help table behavior selected by the new database recipe.
9. Built-in functions, operators, SQL modes, variables, and error/status codes.
10. Hashes for every rowset.

Runtime/generated values such as auto-increment IDs, authentication strings, host/user secrets, UUIDs, server IDs, paths, timestamps, and plugin load state must be generated/redacted by deterministic emulation rules.
