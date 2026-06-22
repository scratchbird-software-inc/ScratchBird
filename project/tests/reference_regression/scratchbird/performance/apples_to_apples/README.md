# ScratchBird Apples-to-Apples Performance Workload

This suite runs the same logical workload as the PostgreSQL apples-to-apples
reference workload against ScratchBird through the public SQL client/parser
route. The engine remains SBLR/UUID-only; SBsql and the parser route own SQL
normalization before engine admission.

Generated data, load scripts, command output, diagnostics, and comparison
artifacts are written under:

```text
build/reference-regression/scratchbird/performance/apples-to-apples/
```

Example:

```bash
python3 project/tests/reference_regression/scratchbird/performance/apples_to_apples/tools/run_scratchbird_apples_to_apples.py \
  --latest-server-json /home/dcalford/CliWork/local_work/scratchbird-driver-test-server/latest.json \
  --scale small \
  --postgres-summary build/reference-regression/postgresql/performance/apples-to-apples/postgresql-small-20260619T230437Z/summary.json
```

The workload records failed statements as evidence. It does not rewrite failed
joins, updates, window functions, CTEs, or aggregate forms into easier
ScratchBird-specific alternatives.

Use `--phase-timing` to include DML phase timing summaries in `summary.json`.
The runner always collects the C++ client trace. Parser-worker and engine DML
trace files are included when the test server was started with the
`server_env_expected` values reported in the phase timing block.

By default the ScratchBird workload prefixes physical table names with
`bench_` to avoid collisions with seeded driver-test objects such as
`app.customers`. The logical workload IDs remain the same as the PostgreSQL
reference suite.
