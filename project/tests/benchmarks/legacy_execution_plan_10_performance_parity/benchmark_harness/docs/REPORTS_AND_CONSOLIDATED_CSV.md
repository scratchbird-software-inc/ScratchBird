# Reports And Consolidated CSV

This document defines where reports are written, what each artifact means, and how to use the unified CSV for comparative decisions.

## Report Artifact Types

For matrix output root `results/matrix-<run-id>`:

- `matrix-summary.json`
  - Matrix-level execution metadata.
- `.matrix-runs.tsv`
  - One line per suite invocation:
  - `engine|suite|started_at|duration_seconds|exit_code|status|output_dir`
- `matrix-comparison-unified.csv`
  - Single CSV with one row per `(suite, metric)` and engine columns.
- `comparison-<suite>/benchmark_comparison_*.txt`
  - Cross-engine text comparison for one suite (`--compare`).
- `comparison-index-comparison/index-comparison-pairwise-*.json`
  - Pairwise target comparison using the normalized index verdict model.
- `comparison-index-comparison/index-comparison-pairwise-*.txt`
  - Human-readable summary of pairwise verdict counts by target.
- `<engine>/<suite>/reports/benchmark_comparison_*.txt`
  - Per-engine suite report (`--report`).
- `<engine>/<suite>/*.json`
  - Raw machine-readable suite outputs.

## How To Read `matrix-summary.json`

High-signal fields:

- `engines_requested`, `suites_requested`
- `total_suite_runs`, `failed_suite_runs`, `result`
- `suite_runs[]` entries:
  - `engine`, `suite`, `status`
  - `duration_seconds`
  - `exit_code`
  - `output_dir`

Use this first to confirm run completeness before deeper analysis.

## How To Read `matrix-comparison-unified.csv`

Columns:

- `run_id`
- `suite`
- `metric`
- one column per engine

Metric families:

- `matrix.*`
  - from matrix runtime metadata:
  - `matrix.status`, `matrix.exit_code`, `matrix.duration_seconds`
- `totals.*`
  - regression summary totals
- `summary.*`
  - suite-specific summary fields
- `summary.by_expectation_status.*`
  - index-comparison scenario expectation states
- `summary.plan_capture_success`
  - successful normalized plan captures for index-comparison
- `results.*` / `test_results.*`
  - derived counts from result payload
- `artifact.result_json`
  - source JSON path for metric provenance

## Decision Workflow Using Consolidated CSV

1. Verify health:
   - Check `matrix.status` and `matrix.exit_code` rows.
2. Verify correctness:
   - Compare `summary.passed`, `summary.failed`, `summary.errors` (or regression `totals.*`).
3. Compare runtime:
   - Use `matrix.duration_seconds` and suite timing summaries.
4. Drill into source:
   - Use `artifact.result_json` to inspect raw suite JSON when a metric differs.

## Practical Queries

Set a csv path:

```bash
csv=results/matrix-<run-id>/matrix-comparison-unified.csv
```

Runtime by suite:

```bash
rg ',matrix.duration_seconds,' "$csv"
```

Stress pass/errors:

```bash
rg ',stress,summary.passed,' "$csv"
rg ',stress,summary.errors,' "$csv"
```

ACID pass/errors:

```bash
rg ',acid,summary.passed,' "$csv"
rg ',acid,summary.errors,' "$csv"
```

Engine differential timing:

```bash
rg ',engine-differential,matrix.duration_seconds,' "$csv"
```

## ScratchBird Comparative Use

When ScratchBird result sets are available, use the same CSV schema per run set and compare:

- baseline native CSV vs ScratchBird-mode CSV
- per suite metric deltas
- status/failure signature differences
- runtime changes by suite and engine mode
- pairwise verdict output from the `index-comparison` lane

This keeps interpretation deterministic and CI-friendly.
