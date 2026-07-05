# System Info And Text Reports

This module provides:

- Optional system metadata collection per run (`system-info.json`)
- Human-readable text report generation (`result_formatter.py`)

## Report Generator

Script:

- `system-info/submit/result_formatter.py`

Help:

```bash
python3 system-info/submit/result_formatter.py --help
```

Common usage:

```bash
# Compare multiple benchmark JSON files into one text report
python3 system-info/submit/result_formatter.py \
  --compare results/matrix-<run-id>/mysql/stress/*.json \
  --output results/matrix-<run-id>/mysql/stress/reports
```

The matrix runner already uses this automatically when `--report` and `--compare` are enabled.

## Where Text Reports Are Written

For matrix output root `results/matrix-<run-id>`:

- Per-engine suite reports:
  - `<engine>/<suite>/reports/benchmark_comparison_*.txt`
- Cross-engine suite reports:
  - `comparison-<suite>/benchmark_comparison_*.txt`

## How To Read Text Reports

Use text reports for quick human review:

- benchmark metadata
- summary pass/fail/error counts
- category-level highlights

For strict automated comparison and decisioning, use:

- `matrix-summary.json`
- `matrix-comparison-unified.csv`

## System Info Collection

System info collection is best-effort and non-blocking in benchmark scripts:

- If system info collection succeeds, `system-info.json` is stored beside suite outputs.
- If it fails, benchmark execution continues and reports are still generated from benchmark JSON.

## Related Docs

- [README.md](project/tests/benchmarks/legacy_execution_plan_10_performance_parity/benchmark_harness/README.md)
- [docs/REPORTS_AND_CONSOLIDATED_CSV.md](project/tests/benchmarks/legacy_execution_plan_10_performance_parity/benchmark_harness/docs/REPORTS_AND_CONSOLIDATED_CSV.md)
