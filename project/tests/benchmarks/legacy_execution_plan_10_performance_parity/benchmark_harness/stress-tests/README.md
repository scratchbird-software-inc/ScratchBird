# Stress Tests

Stress tests exercise multi-table joins, bulk operations, and aggregation-heavy workloads using engine-appropriate SQL.

## Recommended Usage

Via matrix:

```bash
./scripts/run-benchmark-matrix.sh \
  --engines=firebird,mysql,postgresql \
  --suites=stress \
  --report --compare
```

Single engine debug path:

```bash
./scripts/start-engine.sh postgresql start
./scripts/run-benchmark.sh postgresql stress --report
./scripts/start-engine.sh postgresql stop
```

## Scale Control

`scripts/run-benchmark.sh` uses `STRESS_SCALE` (default `medium`):

```bash
STRESS_SCALE=small ./scripts/run-benchmark.sh mysql stress --report
```

## Primary Stress Artifact

- `stress_<engine>_YYYYMMDD_HHMMSS.json`

Key summary fields:

- `summary.total_tests`
- `summary.passed`
- `summary.failed`
- `summary.errors`
- `summary.total_duration_ms`

These appear as `summary.*` rows in `matrix-comparison-unified.csv`.

## Report Locations

Inside matrix output root:

- Per engine text report:
  - `<engine>/stress/reports/benchmark_comparison_*.txt` (`--report`)
- Cross-engine text report:
  - `comparison-stress/benchmark_comparison_*.txt` (`--compare`)

## Related Docs

- [README.md](project/tests/benchmarks/legacy_execution_plan_10_performance_parity/benchmark_harness/README.md)
- [docs/REPORTS_AND_CONSOLIDATED_CSV.md](project/tests/benchmarks/legacy_execution_plan_10_performance_parity/benchmark_harness/docs/REPORTS_AND_CONSOLIDATED_CSV.md)
