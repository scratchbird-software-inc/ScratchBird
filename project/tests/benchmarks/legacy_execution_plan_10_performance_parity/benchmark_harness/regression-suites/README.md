# Regression Suites

Regression suite integration runs upstream test harnesses for:

- Firebird test repository
- MySQL mysql-test
- PostgreSQL pg_regress

## Clone Path Requirements

Set these in `.env` (or environment):

- `FIREBIRD_REPO_PATH`
- `MYSQL_REPO_PATH`
- `POSTGRESQL_REPO_PATH`

Default assumptions are sibling directories of this repo. Use `.env.example` as the template.

## Recommended Usage

Run through matrix orchestrator:

```bash
./scripts/run-benchmark-matrix.sh \
  --engines=firebird,mysql,postgresql \
  --suites=regression \
  --report --compare
```

Or single engine:

```bash
./scripts/start-engine.sh mysql start
./scripts/run-benchmark.sh mysql regression --report
./scripts/start-engine.sh mysql stop
```

## Standalone Regression Runner

You can run direct regression orchestration:

```bash
./regression-suites/run-regression-suite.sh <engine> <target>
```

- `engine`: `firebird|mysql|postgresql|all`
- `target`: `original|scratchbird`

## Output Artifacts

Within matrix output root:

- `<engine>/regression/regression-<engine>-summary.json`
- `<engine>/regression/regression-<engine>.log`
- `<engine>/regression/regression/<engine>/...` (copied raw regression outputs)

`regression-<engine>-summary.json` is the normalized artifact used by consolidated CSV generation.

## Interpretation

High-signal fields in summary:

- `totals.total`
- `totals.passed`
- `totals.failed`
- `totals.errors`
- `totals.pass_rate`

These become `totals.*` rows in `matrix-comparison-unified.csv`.

## Related Docs

- [README.md](project/tests/benchmarks/legacy_execution_plan_10_performance_parity/benchmark_harness/README.md)
- [docs/REPORTS_AND_CONSOLIDATED_CSV.md](project/tests/benchmarks/legacy_execution_plan_10_performance_parity/benchmark_harness/docs/REPORTS_AND_CONSOLIDATED_CSV.md)
