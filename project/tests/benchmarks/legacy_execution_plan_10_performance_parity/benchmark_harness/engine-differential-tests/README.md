# Engine Differential Tests

Differential tests highlight architecture-sensitive behavior across engines and are one of the key inputs for ScratchBird emulation decisions.

## Categories

- MySQL-optimized scenarios
- PostgreSQL-optimized scenarios
- Firebird-optimized scenarios

Primary output is a normalized JSON summary plus per-test results.

## Recommended Usage

Via matrix (preferred):

```bash
SCRATCHBIRD_PG_QUERY_TIMEOUT_MS=30000 \
./scripts/run-benchmark-matrix.sh \
  --engines=firebird,mysql,postgresql \
  --suites=engine-differential \
  --report --compare
```

Single engine debug:

```bash
./scripts/start-engine.sh postgresql start
SCRATCHBIRD_PG_QUERY_TIMEOUT_MS=30000 \
./scripts/run-benchmark.sh postgresql engine-differential --report
./scripts/start-engine.sh postgresql stop
```

## Timeout Control (PostgreSQL)

Use `SCRATCHBIRD_PG_QUERY_TIMEOUT_MS` to bound long-running differential statements:

```bash
SCRATCHBIRD_PG_QUERY_TIMEOUT_MS=30000
```

This is important for consistent matrix completion.

## Primary Differential Artifact

- `differential_<engine>_YYYYMMDD_HHMMSS.json`

High-signal summary fields:

- `summary.total_tests`
- `summary.by_category.mysql_optimized`
- `summary.by_category.pg_optimized`
- `summary.by_category.fb_optimized`

Also compare runtime using matrix metadata row:

- `engine-differential,matrix.duration_seconds`

## Report Locations

Inside matrix output root:

- Per-engine text report:
  - `<engine>/engine-differential/reports/benchmark_comparison_*.txt`
- Cross-engine text report:
  - `comparison-engine-differential/benchmark_comparison_*.txt`

## Related Docs

- [README.md](project/tests/benchmarks/legacy_execution_plan_10_performance_parity/benchmark_harness/README.md)
- [docs/REPORTS_AND_CONSOLIDATED_CSV.md](project/tests/benchmarks/legacy_execution_plan_10_performance_parity/benchmark_harness/docs/REPORTS_AND_CONSOLIDATED_CSV.md)
