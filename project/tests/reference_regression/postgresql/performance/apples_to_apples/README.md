# PostgreSQL Apples-To-Apples Performance Baseline

This package defines the PostgreSQL reference workload used to produce
benchmark numbers for ScratchBird comparison. Outputs are artifacts and are
written under `build/reference-regression/postgresql/performance/apples-to-apples/`.

The suite is intentionally split into:

- deterministic schema and generated data
- PostgreSQL `COPY` load timing for the reference ingest lane
- fixed SQL action files for join, aggregation, insert-select, and update timing

The PostgreSQL results are reference measurements only. ScratchBird must still
execute SQL through SBsql parsing to SBLR/UUID outside the engine, then engine
verification and execution under MGA authority.

## Local Run

```bash
python3 project/tests/reference_regression/postgresql/performance/apples_to_apples/tools/run_postgresql_apples_to_apples.py \
  --host 127.0.0.1 \
  --port 5432 \
  --database sbbench \
  --user sbbench \
  --password sbbench
```

Use `--scale tiny` for a quick harness check. Use `--scale small` for the
legacy parity row shape:

- `customers`: 10,000 rows
- `products`: 5,000 rows
- `orders`: 50,000 rows
- `order_items`: 200,000 rows

The runner records raw query output, timings, row counts, PostgreSQL version,
and `EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON)` for read queries.
