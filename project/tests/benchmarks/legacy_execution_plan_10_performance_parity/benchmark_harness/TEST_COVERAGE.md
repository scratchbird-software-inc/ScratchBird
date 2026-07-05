# Test Coverage Matrix

This file describes what is covered by the current matrix workflow and where artifacts are produced.

## Official Matrix Coverage

Default suites executed by `scripts/run-benchmark-matrix.sh`:

| Suite | Included By Default | Primary Output |
|---|---|---|
| regression | yes | `regression-<engine>-summary.json` |
| stress | yes | `stress_<engine>_*.json` |
| acid | yes | `acid_<engine>_*.json` |
| performance | yes | `performance-<engine>-*.json` |
| tpc-c | yes | `tpc-c-<engine>-*.json` |
| tpc-h | yes | `tpc-h-<engine>-*.json` |
| engine-differential | yes | `differential_<engine>_*.json` |

Official engine set:

- firebird
- mysql
- postgresql

## Matrix-Level Artifacts

For each matrix output root:

- `matrix-summary.json`
- `.matrix-runs.tsv`
- `matrix-comparison-unified.csv`
- optional suite comparison text reports under `comparison-<suite>/`

## Per-Engine Artifact Layout

For each engine and suite:

- `<engine>/<suite>/*.json`
- `<engine>/<suite>/reports/benchmark_comparison_*.txt` (when `--report` enabled)

Regression additionally stores:

- `<engine>/regression/regression-<engine>.log`
- `<engine>/regression/regression-<engine>-summary.json`
- copied raw regression output directory under `<engine>/regression/regression/<engine>/`

## Additional Suites In Repository

The repository also contains additional test areas (for example catalog, protocol, optimizer, ddl, data-type, fault-tolerance) used by specialized runners and development workflows. They are not part of the default matrix unless explicitly wired into matrix execution.

## Coverage Validation Checklist

For a run to be considered complete:

1. `matrix-summary.json` exists and reports expected engine/suite set.
2. `.matrix-runs.tsv` has expected number of rows.
3. All rows are `passed` unless failure is expected/accepted for the test objective.
4. Each requested `<engine>/<suite>` directory contains at least one result JSON.
5. `matrix-comparison-unified.csv` exists and includes rows for every suite.

## Related Docs

- [README.md](project/tests/benchmarks/legacy_execution_plan_10_performance_parity/benchmark_harness/README.md)
- [TEST_STRATEGY.md](project/tests/benchmarks/legacy_execution_plan_10_performance_parity/benchmark_harness/TEST_STRATEGY.md)
- [docs/REPORTS_AND_CONSOLIDATED_CSV.md](project/tests/benchmarks/legacy_execution_plan_10_performance_parity/benchmark_harness/docs/REPORTS_AND_CONSOLIDATED_CSV.md)
