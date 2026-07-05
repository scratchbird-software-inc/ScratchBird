# Benchmark Test Strategy

This strategy defines how benchmark runs are produced and how they are interpreted for native baseline and ScratchBird comparison.

## Primary Goal

Produce comparable, repeatable outputs across:

- FirebirdSQL
- MySQL
- PostgreSQL

Then use the same harness and metric model to evaluate ScratchBird in each dialect mode.

The `index-comparison` lane additionally prepares the harness for:

- upstream target vs upstream target comparisons by normalized index family
- future upstream target vs ScratchBird emulation target comparisons by
  normalized plan and verdict model

## Primary Execution Path

Canonical command:

```bash
SCRATCHBIRD_PG_QUERY_TIMEOUT_MS=30000 \
./scripts/run-benchmark-matrix.sh \
  --engines=firebird,mysql,postgresql \
  --suites=regression,stress,acid,performance,tpc-c,tpc-h,engine-differential,index-comparison \
  --report --compare
```

Rationale:

- Runs one engine at a time for isolation and reproducibility.
- Produces deterministic per-suite outputs and matrix metadata.
- Emits a consolidated CSV (`matrix-comparison-unified.csv`) for direct cross-engine decisions.

## Suite Set In Scope (Matrix Default)

- `regression`
- `stress`
- `acid`
- `performance`
- `tpc-c`
- `tpc-h`
- `engine-differential`
- `index-comparison`

These are the suites used for official matrix comparisons.

## Comparison Model

Decisions are made using the following hierarchy:

1. Run integrity:
   - `matrix-summary.json` result, failed suite count, exit codes.
2. Correctness:
   - regression `totals.*`
   - suite `summary.passed`, `summary.failed`, `summary.errors`
3. Runtime:
   - `matrix.duration_seconds`
   - suite timing fields in `summary.*`
4. Artifact drill-down:
   - inspect source JSON indicated by `artifact.result_json`

For the `index-comparison` suite, also review:

- pairwise verdict artifacts in `comparison-index-comparison/`
- normalized scenario fields such as plan family, fallback rate, and
  order-preservation signals

## Reporting Outputs Required Per Matrix Run

- `matrix-summary.json`
- `.matrix-runs.tsv`
- `matrix-comparison-unified.csv`
- raw suite JSON under `<engine>/<suite>/`
- optional text reports when `--report` and `--compare` are enabled
- pairwise index-comparison artifacts when `index-comparison` is included

## ScratchBird Evaluation Plan

1. Produce native baseline matrix output.
2. Use `index-comparison` to establish per-scenario normalized upstream
   baselines.
3. Produce ScratchBird-mode runs using equivalent suite configuration once the
   ScratchBird benchmark service exists.
4. Generate consolidated CSV for each run set.
5. Compare run health, correctness, runtime, and pairwise verdict outputs.
6. Track deltas over time in CI.

## Quality Controls

- Pin output directories by setting `BENCHMARK_MATRIX_OUTPUT`.
- Keep PostgreSQL differential query timeout bounded (`SCRATCHBIRD_PG_QUERY_TIMEOUT_MS`).
- Validate artifact completeness before analysis.
- Avoid mixing partial and complete runs in one comparison set.
- Treat `execution_status` and `comparative_verdict` as separate concepts.

## Related Docs

- [README.md](project/tests/benchmarks/legacy_execution_plan_10_performance_parity/benchmark_harness/README.md)
- [docs/OPERATIONS_RUNBOOK.md](project/tests/benchmarks/legacy_execution_plan_10_performance_parity/benchmark_harness/docs/OPERATIONS_RUNBOOK.md)
- [docs/REPORTS_AND_CONSOLIDATED_CSV.md](project/tests/benchmarks/legacy_execution_plan_10_performance_parity/benchmark_harness/docs/REPORTS_AND_CONSOLIDATED_CSV.md)
- [index-comparison-tests/README.md](project/tests/benchmarks/legacy_execution_plan_10_performance_parity/benchmark_harness/index-comparison-tests/README.md)
