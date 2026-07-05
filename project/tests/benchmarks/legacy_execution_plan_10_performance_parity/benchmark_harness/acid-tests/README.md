# ACID Tests

ACID tests cover transaction semantics and constraint behavior with per-engine result JSON suitable for matrix comparison.

## Recommended Usage

Via matrix:

```bash
./scripts/run-benchmark-matrix.sh \
  --engines=firebird,mysql,postgresql \
  --suites=acid \
  --report --compare
```

Single engine debug:

```bash
./scripts/start-engine.sh firebird start
./scripts/run-benchmark.sh firebird acid --report
./scripts/start-engine.sh firebird stop
```

## Standalone Script

```bash
./acid-tests/scripts/run-acid-tests.sh <engine> [category]
```

- `engine`: `firebird|mysql|postgresql|all`
- `category` optional: `atomicity|consistency|isolation|durability`

## Primary ACID Artifact

- `acid_<engine>_YYYYMMDD_HHMMSS.json`

Expected keys:

- `metadata`
- `results` (category-level arrays)
- `summary`

High-signal summary metrics:

- `summary.total`
- `summary.passed`
- `summary.failed`
- `summary.errors`
- `summary.by_category.*`

These are emitted into `matrix-comparison-unified.csv` as `summary.*` and `results.*` rows.

## Report Locations

Inside matrix output root:

- Per-engine text report:
  - `<engine>/acid/reports/benchmark_comparison_*.txt`
- Cross-engine text report:
  - `comparison-acid/benchmark_comparison_*.txt`

## Related Docs

- [README.md](project/tests/benchmarks/legacy_execution_plan_10_performance_parity/benchmark_harness/README.md)
- [docs/REPORTS_AND_CONSOLIDATED_CSV.md](project/tests/benchmarks/legacy_execution_plan_10_performance_parity/benchmark_harness/docs/REPORTS_AND_CONSOLIDATED_CSV.md)
