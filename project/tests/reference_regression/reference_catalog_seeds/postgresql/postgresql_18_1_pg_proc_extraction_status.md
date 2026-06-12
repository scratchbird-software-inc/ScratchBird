# PostgreSQL 18.1 pg_proc extraction status

Search key: `SB_REFERENCE_POSTGRESQL_18_1_PG_PROC_EXTRACTION_STATUS`

Status: incomplete, extraction required before implementation-ready function/operator/routine emulation.

Private evidence now available:

1. `project/tests/reference_regression/reference_emulation_source_summaries/postgresql-18.1/50_catalog.md`.
2. `project/tests/reference_regression/reference_emulation_source_summaries/postgresql-18.1/60_builtins.md`.

Additional source evidence observed:

1. PostgreSQL stores initial builtin procedure/function rows in `pg_proc.dat` style records.
2. Each row includes fields such as `oid`, `descr`, `proname`, volatility/leakproof/support flags where present, `prorettype`, `proargtypes`, and `prosrc`.
3. Some entries are direct-call functions; some are I/O routines, operators' implementation functions, planner support functions, casts, aggregate transition/final functions, or catalog support functions.
4. `pronargs` is computed by catalog tooling and must be represented as a generated value rule in the seed manifest rather than copied blindly.

Required extraction outputs:

1. Complete normalized `pg_proc` rowset for PostgreSQL 18.1.
2. Complete `pg_operator` rowset and link to function implementation rows.
3. Complete `pg_aggregate` rowset and transition/final/combine/serialize/deserial rows.
4. Complete cast/function/operator mapping to native v3 or C++ UDR/refusal.
5. Volatility, leakproof, strictness, parallel-safety, security-definer, support-function, and cost/rows metadata mapping.
6. Exact rowset hashes.

Invalid implementation-ready claim diagnostic: `POSTGRESQL.FUNCTION_MAPPING_MISSING`.
