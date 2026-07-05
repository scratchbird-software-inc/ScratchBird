# Operations Runbook

This runbook is the reproducible workflow for collecting native baseline metrics and preparing data for ScratchBird comparison.

## 1) Prerequisites

- Docker daemon access:
  - `docker info` must succeed as your current user.
- Python 3.8+:
  - `python3 --version`
- Repo paths configured for regression suite:
  - `FIREBIRD_REPO_PATH`
  - `MYSQL_REPO_PATH`
  - `POSTGRESQL_REPO_PATH`

Initialize local config:

```bash
cd project/tests/benchmarks/legacy_execution_plan_10_performance_parity/benchmark_harness
cp .env.example .env
```

## 2) Recommended Baseline Run

```bash
cd project/tests/benchmarks/legacy_execution_plan_10_performance_parity/benchmark_harness

SCRATCHBIRD_PG_QUERY_TIMEOUT_MS=30000 \
./scripts/run-benchmark-matrix.sh \
  --engines=firebird,mysql,postgresql \
  --suites=regression,stress,acid,performance,tpc-c,tpc-h,engine-differential \
  --report --compare
```

Notes:

- `SCRATCHBIRD_PG_QUERY_TIMEOUT_MS` keeps PostgreSQL differential queries bounded.
- Add `BENCHMARK_MATRIX_OUTPUT=results/matrix-<name>` to force stable output path names.

## 3) Single Engine / Suite Execution

Use this when debugging a failing suite:

```bash
./scripts/start-engine.sh mysql start
./scripts/run-benchmark.sh mysql stress --report --output results/debug-mysql-stress
./scripts/start-engine.sh mysql stop
```

## 4) Output Integrity Checklist

Given matrix root `results/matrix-<run-id>`:

1. `matrix-summary.json` exists and `result` is `passed`.
2. `.matrix-runs.tsv` has one row per requested `(engine,suite)`.
3. `matrix-comparison-unified.csv` exists.
4. Every requested engine has JSON in every requested suite directory.
5. `comparison-<suite>/benchmark_comparison_*.txt` exists when `--compare` was enabled.

## 5) Rebuild Consolidated CSV

If you already have a matrix root:

```bash
python3 scripts/generate-unified-comparison-csv.py \
  --output-root results/matrix-<run-id>
```

Or with explicit summary path:

```bash
python3 scripts/generate-unified-comparison-csv.py \
  --summary results/matrix-<run-id>/matrix-summary.json \
  --output results/matrix-<run-id>/matrix-comparison-unified.csv
```

## 6) Troubleshooting

- Docker permission errors:
  - Add user to docker group or run with `sudo`.
- Port conflicts:
  - Set `BENCHMARK_FIREBIRD_PORT`, `BENCHMARK_MYSQL_PORT`, `BENCHMARK_POSTGRESQL_PORT`.
  - `start-engine.sh` also auto-increments to free ports.
- Regression path failures:
  - Confirm `.env` repo path variables point to valid local clones.
- Partial matrix:
  - Use `.matrix-runs.tsv` plus `matrix-summary.json` to identify the failed suite row.
